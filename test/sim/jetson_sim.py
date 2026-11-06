#!/usr/bin/env python3
"""Drive the host-simulated firmware the way AURA-2026/brain does.

Uses the REAL CommandSender from AURA-2026/brain/command_sender.py and mimics
dashboard_server.py: M:PING every 50 ms, arm on EVENT:SYSTEM_READY, re-send the
last speed on EVENT:OBSTACLE_CLEARED.
"""
import importlib.util
import re
import subprocess
import sys
import threading
import time

import os
AURA_BRAIN = os.environ.get("AURA_BRAIN") or os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "AURA-2026", "brain")
BRAIN = os.path.join(AURA_BRAIN, "command_sender.py")
if not os.path.isfile(BRAIN):
    sys.exit(f"AURA-2026 not found at {AURA_BRAIN}: set AURA_BRAIN=/path/to/AURA-2026/brain")
spec = importlib.util.spec_from_file_location("command_sender", BRAIN)
cs_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cs_mod)
CommandSender = cs_mod.CommandSender
duty = cs_mod.ui_speed_to_duty

LINE_RE = re.compile(r"^t=(\d+) (.*)$")


class Car:
    def __init__(self, binary, arm_on_ready=True):
        self.p = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1)
        self.lines = []  # (fw_ms, text)
        self.cv = threading.Condition()
        self.pwm, self.dir, self.servo = 0, "BRAKE", None
        self.state = None
        self.violations = []
        self.heartbeat = True
        self.arm_on_ready = arm_on_ready
        self.wlock = threading.Lock()
        self.cs = CommandSender(self._write)
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._hb, daemon=True).start()

    # -- link ------------------------------------------------------------
    def _write(self, cmd):
        with self.wlock:
            try:
                self.p.stdin.write(cmd if cmd.endswith("\n") else cmd + "\n")
                self.p.stdin.flush()
            except BrokenPipeError:
                return False, "closed"
        return True, "OK"

    def raw(self, line):
        self._write(line)

    def sim(self, cmd):
        self._write("#SIM " + cmd)

    def _hb(self):
        while self.p.poll() is None:
            if self.heartbeat:
                self.cs.send_heartbeat()
            time.sleep(0.05)

    def _reader(self):
        for raw in self.p.stdout:
            m = LINE_RE.match(raw.rstrip("\n"))
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
                ev = text[4:]
                # dashboard_server.parse_system_events
                if ev.startswith("EVENT:STATE_CHANGED:"):
                    self.state = ev.split(":")[2].split()[0]
                elif ev.startswith("EVENT:SYSTEM_READY"):
                    self.cs.reset_link_state()
                    if self.arm_on_ready:
                        self.cs.send_arm()
                elif ev.startswith("EVENT:OBSTACLE_CLEARED"):
                    self.cs.resend_last_speed()
            with self.cv:
                self.lines.append((t, text))
                self.cv.notify_all()

    # -- helpers ---------------------------------------------------------
    def now(self):
        with self.cv:
            return self.lines[-1][0] if self.lines else 0

    def mark(self):
        with self.cv:
            return len(self.lines)

    def wait(self, pattern, since=0, timeout=2.0):
        """First line matching `pattern` after index `since`: (fw_ms, text) or None."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        with self.cv:
            while True:
                for t, text in self.lines[since:]:
                    if rx.search(text):
                        return t, text
                left = end - time.time()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def seen(self, pattern, since=0):
        rx = re.compile(pattern)
        with self.cv:
            return [l for l in self.lines[since:] if rx.search(l[1])]

    def boot(self):
        assert self.wait(r"EVENT:SYSTEM_READY", timeout=3), "no SYSTEM_READY"
        if self.arm_on_ready:
            assert self.wait(r"STATE_CHANGED:RUNNING", timeout=2), "never RUNNING after arm"
        time.sleep(0.1)

    def close(self):
        try:
            self.sim("quit")
        except Exception:
            pass
        self.p.wait(timeout=2)


def settle(s=0.15):
    time.sleep(s)


# ---------------------------------------------------------------------------
# Scenarios. Each returns a list of (check, ok, detail).
# ---------------------------------------------------------------------------
def sc_boot_arm(car):
    car.boot()
    return [("boot: SYSTEM_READY -> SYS_ARM -> RUNNING", car.state == "RUNNING", car.state),
            ("boot: motor still, servo centered", car.pwm == 0 and car.servo == 105, f"pwm={car.pwm} servo={car.servo}")]


def sc_cruise(car):
    car.boot()
    car.cs.send_speed_command(200)
    settle()
    first = car.pwm
    time.sleep(2.0)  # only M:PING from now on
    return [("cruise: SET_SPEED applied as duty", first == duty(200), f"pwm={first} expected={duty(200)}"),
            ("cruise: speed held 2 s with only PINGs", car.pwm == duty(200) and car.dir == "F", f"pwm={car.pwm} dir={car.dir}")]


def sc_steer(car):
    car.boot()
    car.cs.send_steering_command(70)
    settle()
    a = car.servo
    car.cs.send_steering_command(300)  # out of range: CommandSender clamps too
    settle()
    car.raw("C:SET_STEER:-30")  # raw negative: firmware must clamp to LEFT, not wrap
    settle()
    return [("steer: angle applied", a == 70, f"servo={a}"),
            ("steer: raw -30 clamps to 50 (no wrap)", car.servo == 50, f"servo={car.servo}")]


def sc_stop_start_speed0(car):
    """stop_strategy.py: SET_SPEED:0, wait, schedule_speed_resume."""
    car.boot()
    car.cs.send_speed_command(150)
    settle()
    moving = car.pwm
    car.cs.send_speed_command(0)
    settle()
    stopped = car.pwm
    time.sleep(0.5)
    car.cs.send_speed_command(150)
    settle()
    return [("stop sign: SET_SPEED:0 stops", moving > 0 and stopped == 0, f"{moving}->{stopped}"),
            ("stop sign: resume speed moves again", car.pwm == duty(150), f"pwm={car.pwm}")]


def sc_obstacle(car):
    car.boot()
    car.cs.send_speed_command(200)
    car.cs.send_steering_command(105)
    settle()
    m = car.mark()
    car.sim("dist 20")
    det = car.wait(r"USB EVENT:OBSTACLE_DETECTED", since=m)
    stop = car.wait(r"#HW motor .*pwm=0", since=m)
    settle(0.1)
    state_stopped = car.state
    # Jetson steers while stopped (autopilot sends only changes)
    car.cs.send_steering_command(80)
    settle()
    servo_during = car.servo
    car.cs.send_speed_command(220)  # ignored while stopped, but remembered by CommandSender
    settle()
    pwm_during = car.pwm
    m2 = car.mark()
    car.sim("dist 120")
    clr = car.wait(r"USB EVENT:OBSTACLE_CLEARED", since=m2, timeout=3)
    settle(0.3)
    uart = car.seen(r"UART EVENT:OBSTACLE_DETECTED")
    lat = (stop[0] - m and stop[0]) if stop else None
    return [("obstacle: OBSTACLE_DETECTED emitted", det is not None, det[1] if det else None),
            ("obstacle: event also on UART (Serial1)", bool(uart), f"{len(uart)} lines"),
            ("obstacle: motor braked", stop is not None and (det is None or stop[0] - det[0] < 30),
             f"brake {stop[0] - det[0] if stop and det else '?'} ms after event"),
            ("obstacle: state STOPPED", state_stopped == "STOPPED", state_stopped),
            ("obstacle: servo still follows the Jetson (only forward is blocked)", servo_during == 80, f"servo={servo_during}"),
            ("obstacle: SET_SPEED ignored while stopped", pwm_during == 0, f"pwm={pwm_during}"),
            ("obstacle: cleared -> Jetson resend_last_speed -> moving", clr is not None and car.pwm == duty(220), f"pwm={car.pwm}"),
            ("obstacle: steer asked during stop applied on resume", car.servo == 80, f"servo={car.servo}")]


def sc_link_loss(car):
    car.boot()
    car.cs.send_speed_command(200)
    car.cs.send_steering_command(70)
    settle()
    m = car.mark()
    t0 = car.now()
    car.heartbeat = False
    wd = car.wait(r"EVENT:WATCHDOG_TIMEOUT", since=m, timeout=2)
    settle()
    res = [("link loss: WATCHDOG_TIMEOUT", wd is not None, f"after {wd[0] - t0} ms" if wd else None),
           ("link loss: FAULT, motor stopped, servo centered",
            car.state == "FAULT" and car.pwm == 0 and car.servo == 105, f"{car.state} pwm={car.pwm} servo={car.servo}")]
    car.heartbeat = True
    car.cs.send_arm()
    settle(0.3)
    res.append(("link loss: re-arm -> RUNNING but stays still", car.state == "RUNNING" and car.pwm == 0, f"{car.state} pwm={car.pwm}"))
    return res


def sc_garbage_no_feed(car):
    car.boot()
    car.heartbeat = False
    stop = time.time() + 0.8
    while time.time() < stop:
        car.raw("hello garbage")
        car.raw("M:LIGHTS_ON:0")
        time.sleep(0.03)
    return [("garbage/unknown lines do not feed watchdog", car.state == "FAULT", car.state)]


def sc_reverse(car):
    """parking_strategy: forward then backward with latched speed, 20 Hz."""
    car.boot()
    m = car.mark()
    car.cs.send_speed_command(180, "forward")
    settle(0.3)
    car.cs.send_speed_command(180, "backward")
    settle(0.3)
    dt = car.seen(r"#HW reverse deadtime", since=m)
    return [("reverse: no plugging (flip with PWM on)", not car.violations, "; ".join(car.violations) or "none"),
            ("reverse: dead time >= 60 ms", bool(dt) and int(dt[-1][1].split("=")[1]) >= 60, dt[-1][1] if dt else "no flip seen"),
            ("reverse: ends backward at speed", car.dir == "B" and car.pwm == duty(180), f"dir={car.dir} pwm={car.pwm}")]


def sc_parking_burst(car):
    """parking_strategy._set_motion at 20 Hz: SET_DIR?+SET_SPEED+SET_STEER every 50 ms + PING."""
    car.boot()
    phases = [(0, "forward", 105, 0.4), (120, "forward", 160, 0.8), (120, "backward", 50, 0.8),
              (120, "backward", 160, 0.8), (0, "forward", 105, 0.4)]
    for speed, d, servo, dur in phases:
        end = time.time() + dur
        while time.time() < end:
            car.cs.send_speed_command(speed, d)
            car.cs.send_steering_command(servo)
            time.sleep(0.05)
    settle(0.2)
    return [("parking: no plugging across phases", not car.violations, "; ".join(car.violations) or "none"),
            ("parking: final stop, centered", car.pwm == 0 and car.servo == 105, f"pwm={car.pwm} servo={car.servo}"),
            ("parking: link stayed up", car.state == "RUNNING", car.state)]


def sc_brake_now(car):
    """Dashboard 'brake' button -> send_brake() -> E:BRAKE_NOW."""
    car.boot()
    car.cs.send_speed_command(200)
    settle()
    m = car.mark()
    car.cs.send_brake()
    stop = car.wait(r"#HW motor .*pwm=0", since=m)
    settle()
    car.cs.send_speed_command(200)  # user presses forward right after
    settle(0.3)
    after_03 = car.pwm
    time.sleep(1.0)
    car.cs.send_speed_command(200)
    settle()
    return [("brake: motor stops", stop is not None, ""),
            ("brake: forward 0.3 s later moves", after_03 > 0, f"pwm={after_03}"),
            ("brake: forward 1.3 s later moves", car.pwm > 0, f"pwm={car.pwm}")]


def sc_sys_stop(car):
    car.boot()
    car.cs.send_speed_command(200)
    settle()
    car.raw("M:SYS_STOP:0")
    settle()
    s1, p1 = car.state, car.pwm
    car.cs.send_speed_command(200)
    settle()
    p2 = car.pwm
    car.raw("M:SYS_RESUME:0")
    settle()
    p3 = car.pwm
    car.cs.send_speed_command(200)
    settle()
    return [("SYS_STOP: STOPPED and braked", s1 == "STOPPED" and p1 == 0, f"{s1} pwm={p1}"),
            ("SYS_STOP: SET_SPEED ignored while latched", p2 == 0, f"pwm={p2}"),
            ("SYS_RESUME: does not restart by itself", p3 == 0 and car.state == "RUNNING", f"pwm={p3} {car.state}"),
            ("SYS_RESUME: new SET_SPEED moves", car.pwm == duty(200), f"pwm={car.pwm}")]


def sc_estop(car):
    car.boot()
    car.cs.send_speed_command(200)
    settle()
    car.sim("estop 1")
    settle(0.2)
    s1, p1 = car.state, car.pwm
    m = car.mark()
    car.cs.send_arm()
    rej = car.wait(r"CMD_REJECTED:SYS_ARM", since=m, timeout=0.5)
    car.sim("estop 0")
    settle(0.1)
    car.cs.send_arm()
    settle(0.2)
    return [("E-STOP: FAULT + braked", s1 == "FAULT" and p1 == 0, f"{s1} pwm={p1}"),
            ("E-STOP: arm rejected while pressed", rej is not None, ""),
            ("E-STOP: released -> arm -> RUNNING", car.state == "RUNNING", car.state)]


def sc_disarm(car):
    car.boot()
    car.cs.send_speed_command(200)
    car.cs.send_steering_command(70)
    settle()
    car.cs.send_disarm()
    settle()
    s, p, a = car.state, car.pwm, car.servo
    car.cs.send_speed_command(200)
    car.cs.send_steering_command(150)
    settle()
    return [("DISARM: stopped + centered", s == "DISARMED" and p == 0 and a == 105, f"{s} pwm={p} servo={a}"),
            ("DISARM: commands ignored", car.pwm == 0 and car.servo == 105, f"pwm={car.pwm} servo={car.servo}")]


def sc_stale_steer_after_fault(car):
    """Autopilot dedupes SET_STEER: after FAULT+re-arm the servo is centered but
    the Jetson still believes it is at the old angle (Jetson-side issue)."""
    car.boot()
    last = None
    def autopilot(angle):
        nonlocal last
        if angle != last:
            car.cs.send_steering_command(angle)
            last = angle
    autopilot(70)
    settle()
    car.heartbeat = False
    car.wait(r"WATCHDOG_TIMEOUT", timeout=2)
    car.heartbeat = True
    car.cs.send_arm()
    settle(0.3)
    for _ in range(10):  # PID keeps computing 70
        autopilot(70)
        time.sleep(0.033)
    return [("Jetson: steer after FAULT re-arm matches PID (known Jetson-side gap)", car.servo == 70, f"servo={car.servo} (Jetson thinks 70)")]


SCENARIOS = [sc_boot_arm, sc_cruise, sc_steer, sc_stop_start_speed0, sc_obstacle, sc_link_loss,
             sc_garbage_no_feed, sc_reverse, sc_parking_burst, sc_brake_now, sc_sys_stop, sc_estop,
             sc_disarm, sc_stale_steer_after_fault]


def run(binary):
    results = []
    for sc in SCENARIOS:
        car = Car(binary)
        try:
            results += sc(car)
        except AssertionError as e:
            results.append((sc.__name__, False, f"aborted: {e}"))
        finally:
            car.close()
    return results


if __name__ == "__main__":
    out = {}
    for b in sys.argv[1:]:
        out[b] = run(b)
    names = [r[0] for r in out[sys.argv[1]]]
    for i, n in enumerate(names):
        row = []
        for b in sys.argv[1:]:
            r = out[b][i] if i < len(out[b]) else (n, False, "missing")
            row.append(("PASS" if r[1] else "FAIL") + f" ({r[2]})")
        print(f"{n}\n    " + "\n    ".join(f"{b}: {x}" for b, x in zip(sys.argv[1:], row)))
