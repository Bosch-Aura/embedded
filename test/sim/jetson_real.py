#!/usr/bin/env python3
"""Run the scenarios with the REAL Jetson event handling and autopilot dedupe.

parse_system_events (dashboard_server.py) and the steering methods of
AutoPilotController are pulled out of the source with `ast` and executed
against stand-in globals, so Flask/OpenCV/camera are not needed.
"""
import ast
import sys
import threading
import time

import jetson_sim as js

BRAIN = js.AURA_BRAIN


def extract(path, names, cls=None, optional=()):
    tree = ast.parse(open(path).read())
    body = tree.body
    if cls:
        body = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == cls).body
    found = [n for n in body if isinstance(n, ast.FunctionDef) and n.name in list(names) + list(optional)]
    missing = set(names) - {n.name for n in found}
    assert not missing, f"missing {sorted(missing)} in {path}"
    return ast.unparse(ast.Module(body=found, type_ignores=[]))


def module_helpers(path, root):
    """Top-level functions `root` calls, directly or through each other."""
    funcs = {n.name: n for n in ast.parse(open(path).read()).body if isinstance(n, ast.FunctionDef)}
    todo, seen = [root], set()
    while todo:
        name = todo.pop()
        if name in seen or name not in funcs:
            continue
        seen.add(name)
        todo += [n.id for n in ast.walk(funcs[name]) if isinstance(n, ast.Name)]
    return sorted(seen - {root})


DASHBOARD = f"{BRAIN}/dashboard/dashboard_server.py"
EVENTS_SRC = extract(DASHBOARD, ["parse_system_events"], optional=module_helpers(DASHBOARD, "parse_system_events"))
AP_SRC = extract(f"{BRAIN}/lane_detection/autopilot_controller.py",
                 ["_send_servo_if_changed", "invalidate_steering"], cls="AutoPilotController",
                 optional=["resume"])


class _Resettable:
    def reset(self):
        pass


class Autopilot:
    """AutoPilotController with only its real steering methods (and resume)."""
    def __init__(self, cs):
        self.command_sender = cs
        self.lock = threading.Lock()
        self.is_paused = False
        self.pid_controller = self.filter_controller = _Resettable()
        self.last_servo_angle = None
        self.last_steering_angle = None
        self.command_count = self.error_count = 0


ns = {}
exec(AP_SRC, ns)
Autopilot._send_servo_if_changed = ns["_send_servo_if_changed"]
Autopilot.invalidate_steering = ns["invalidate_steering"]
if "resume" in ns:
    Autopilot.resume = ns["resume"]


class RealCar(js.Car):
    def __init__(self, binary, arm_on_ready=True):
        self.g = None
        super().__init__(binary, arm_on_ready)
        self.ap = Autopilot(self.cs)
        self.g = {
            "time": time, "sys": sys, "print": lambda *a, **k: None,
            "system_state": {"mode": "MANUAL", "state": "DISARMED"},
            "system_state_lock": threading.Lock(),
            "esp32_ready_event": threading.Event(),
            "command_sender": self.cs, "autopilot_controller": self.ap,
            "sign_detection_controller": None,
            "ultra_status_lock": threading.Lock(),
            "ultra_last_status": {"ultra_blocking": False, "dist_cm": None, "last_event_age_ms": None},
            "ultra_last_event_time": None, "ultra_safety_enabled": True,
            "esp32_speed": {"duty": None, "held": None, "rejected": None},
            "esp32_speed_lock": threading.Lock(),
        }
        exec(EVENTS_SRC, self.g)

    def on_event(self, ev):
        # Track state for the checks, then hand the line to the real handler.
        if ev.startswith("EVENT:STATE_CHANGED:"):
            self.state = ev.split(":")[2].split()[0]
        while self.g is None:
            time.sleep(0.001)
        try:
            self.g["parse_system_events"](ev)
        except Exception as e:  # a broken handler must fail the tests, not hide
            self.violations.append(f"HANDLER ERROR on {ev!r}: {e!r}")
            print(f"HANDLER ERROR on {ev!r}: {e!r}", file=sys.stderr, flush=True)


# Patch the reader so USB EVENT lines go to the real handler.
def _reader(self):
    for raw in self.p.stdout:
        m = js.LINE_RE.match(raw.rstrip("\n"))
        if not m:
            continue
        t, text = int(m.group(1)), m.group(2)
        if text.startswith("#HW motor"):
            d = dict(kv.split("=") for kv in text.split()[2:])
            self.dir, self.pwm = d["dir"], int(d["pwm"])
        elif text.startswith("#HW servo="):
            self.servo = int(text.split("=")[1])
        elif "VIOLATION" in text:
            self.violations.append(text)
        elif text.startswith("USB EVENT:"):
            self.on_event(text[4:])
        with self.cv:
            self.lines.append((t, text))
            self.cv.notify_all()


RealCar._reader = _reader


def run_autopilot(car, angle, cycles=10):
    for _ in range(cycles):
        car.ap._send_servo_if_changed(0.0, angle)  # PID output constant
        time.sleep(0.033)


def sc_rearm_after_fault(car):
    car.boot()
    run_autopilot(car, 70)
    car.heartbeat = False
    car.wait(r"WATCHDOG_TIMEOUT", timeout=2)
    js.settle()
    centered = car.servo
    car.heartbeat = True
    car.cs.send_arm()
    js.settle(0.3)
    run_autopilot(car, 70)
    return [("FAULT: servo centered by firmware", centered == 105, f"servo={centered}"),
            ("FAULT re-arm: autopilot re-sends its angle", car.servo == 70, f"servo={car.servo}")]


def sc_rearm_after_disarm(car):
    car.boot()
    run_autopilot(car, 140)
    car.cs.send_disarm()
    js.settle()
    run_autopilot(car, 140)  # while DISARMED: nothing sent (unchanged) / ignored
    centered = car.servo
    car.cs.send_arm()
    js.settle(0.3)
    run_autopilot(car, 140)
    return [("DISARM: servo centered", centered == 105, f"servo={centered}"),
            ("DISARM re-arm: autopilot re-sends its angle", car.servo == 140, f"servo={car.servo}")]


def sc_reboot(car):
    """Port reopened / board rebooted: SYSTEM_READY -> arm -> RUNNING."""
    car.boot()
    run_autopilot(car, 90)
    ok_before = car.servo == 90
    car.close()
    car2 = RealCar(car.p.args[0])
    car2.ap.last_servo_angle = 90  # the Jetson process survives the board reboot
    car2.boot()
    run_autopilot(car2, 90)
    servo = car2.servo
    car2.close()
    return [("reboot: autopilot re-sends after SYSTEM_READY+arm", ok_before and servo == 90, f"servo={servo}")]


def sc_obstacle_real(car):
    """Real OBSTACLE_CLEARED handler re-sends speed; steering while stopped."""
    car.boot()
    car.cs.send_speed_command(200)
    run_autopilot(car, 105, 3)
    car.sim("dist 20")
    car.wait(r"OBSTACLE_DETECTED", timeout=2)
    js.settle()
    run_autopilot(car, 85)
    during = (car.pwm, car.servo)
    car.sim("dist 150")
    car.wait(r"OBSTACLE_CLEARED", timeout=3)
    js.settle(0.3)
    return [("obstacle (real handler): stopped, servo follows", during == (0, 85), f"pwm,servo={during}"),
            ("obstacle (real handler): resumes speed + applies angle",
             car.pwm == js.duty(200) and car.servo == 85, f"pwm={car.pwm} servo={car.servo}"),
            ("obstacle (real handler): Jetson saw ultra_blocking cleared",
             car.g["ultra_last_status"]["ultra_blocking"] is False, str(car.g["ultra_last_status"]))]


def sc_no_resend_while_running(car):
    """Invalidation must not spam: STOPPED<->RUNNING must not re-send."""
    car.boot()
    run_autopilot(car, 100, 3)
    sent_before = car.ap.command_count
    car.raw("M:SYS_STOP:0")
    js.settle()
    car.raw("M:SYS_RESUME:0")
    js.settle()
    run_autopilot(car, 100, 5)
    return [("STOPPED->RUNNING does not invalidate (no extra send)",
             car.ap.command_count == sent_before, f"sends {sent_before}->{car.ap.command_count}")]


SCENARIOS = [sc_rearm_after_fault, sc_rearm_after_disarm, sc_reboot, sc_obstacle_real,
             sc_no_resend_while_running]

if __name__ == "__main__":
    binary = sys.argv[1]
    results = []
    # The generic scenarios too, but through the real event handler.
    generic = [s for s in js.SCENARIOS if s is not js.sc_stale_steer_after_fault]  # replaced by the real-code ones
    for sc in generic + SCENARIOS:
        car = RealCar(binary)
        try:
            results += sc(car)
        except AssertionError as e:
            results.append((sc.__name__, False, f"aborted: {e}"))
        finally:
            try:
                car.close()
            except Exception:
                pass
    fails = 0
    for name, ok, detail in results:
        fails += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}  ({detail})")
    print(f"\n{len(results) - fails}/{len(results)} passed")
    sys.exit(1 if fails else 0)
