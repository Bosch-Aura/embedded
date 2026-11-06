#!/usr/bin/env python3
"""Jetson-side regressions against the simulated firmware.

Uses the REAL AURA-2026 code: CommandSender, parse_system_events and
translate_http_to_uart from dashboard_server.py (extracted with ast), the
autopilot steering methods, and SignController with its real strategies
(only the YOLO detector is stubbed).

    python3 jetson_regressions.py build/fw_sim
"""
import contextlib
import io
import sys
import time
import types

import jetson_sim as js
from jetson_real import RealCar, extract, run_autopilot

sys.path.insert(0, js.AURA_BRAIN)
_stub = types.ModuleType("sign_vision.sign_detector")


class SignDetector:
    confidence_threshold = 0.5

    def __init__(self):
        self.dets = []

    def get_detections(self):
        d, self.dets = self.dets, []
        return d


_stub.SignDetector = SignDetector
sys.modules["sign_vision.sign_detector"] = _stub
from sign_vision.sign_controller import SignController  # noqa: E402

BIN = sys.argv[1]
TRANSLATE = extract(f"{js.AURA_BRAIN}/dashboard/dashboard_server.py", ["translate_http_to_uart"])
STOP_SIGN = {"class": "stop", "confidence": 0.9, "distance": 0.3}
quiet = contextlib.redirect_stdout(io.StringIO())
results = []


def report(name, ok, detail):
    results.append((name, ok, detail))
    print(f"{'PASS' if ok else 'INFO' if ok is None else 'FAIL'}  {name}  ({detail})", flush=True)


def route(car, endpoint, args=None):
    """Dashboard HTTP route -> real translate_http_to_uart -> action."""
    exec(TRANSLATE, car.g)
    ok, msg, action = car.g["translate_http_to_uart"](endpoint, args or {})
    return ok, msg, (action(car.cs) if action else None)


def new_car():
    car = RealCar(BIN)
    car.sim("dist 200")
    car.boot()
    exec(TRANSLATE, car.g)
    return car


def with_signs():
    car = new_car()
    det = SignDetector()
    sc = SignController(det, car.cs)
    car.g["sign_detection_controller"] = sc
    with quiet:
        sc.start()
    route(car, "changeSpeed", {"speed": "200", "direction": "forward"})
    sc.update_current_speed(200)
    js.settle(0.3)
    return car, det, sc


def obstacle_passes(car):
    car.sim("dist 20")
    car.wait(r"OBSTACLE_DETECTED", timeout=2)
    js.settle()
    car.sim("dist 150")
    car.wait(r"OBSTACLE_CLEARED", timeout=3)
    js.settle(0.3)


# R1 stale last_speed after watchdog FAULT -------------------------------------
car = new_car()
car.cs.send_speed_command(200)
js.settle(0.3)
car.heartbeat = False
car.wait("WATCHDOG_TIMEOUT", timeout=2)
js.settle()
car.heartbeat = True
car.cs.send_arm()
js.settle(0.3)
obstacle_passes(car)
report("R1 FAULT -> re-arm -> someone walks past: car stays still", car.pwm == 0,
       f"pwm={car.pwm} last_speed={car.cs.last_speed}")
car.close()

# R2 same after a board reboot (the Jetson process survives) -------------------
car = new_car()
car.cs.send_speed_command(200)
js.settle(0.3)
speed, direction = car.cs.last_speed, car.cs.last_direction
car.close()
car = RealCar(BIN)
car.cs.last_speed, car.cs.last_direction = speed, direction
car.sim("dist 200")
car.boot()
obstacle_passes(car)
report("R2 reboot + auto-arm -> obstacle passes: car stays still", car.pwm == 0,
       f"pwm={car.pwm} last_speed={car.cs.last_speed}")
car.close()

# R3 obstacle that ends a real block still resumes ----------------------------
car = new_car()
car.cs.send_speed_command(200)
js.settle(0.3)
obstacle_passes(car)
report("R3 normal obstacle stop -> clear -> resumes (unchanged)", car.pwm == js.duty(200), f"pwm={car.pwm}")
car.close()

# R4 OBSTACLE_RESET with the obstacle still there: no lurch -------------------
car = new_car()
car.cs.send_speed_command(200)
js.settle(0.3)
car.sim("dist 20")
car.wait(r"OBSTACLE_DETECTED", timeout=2)
js.settle(0.3)
m = car.mark()
route(car, "obstacleReset")
car.wait(r"OBSTACLE_DETECTED", since=m, timeout=2)
js.settle(0.1)
fwd = [x for _, x in car.seen(r"#HW motor dir=F pwm=[1-9]", m)]
report("R4 RESET with obstacle at 20 cm: no forward PWM before re-block", not fwd, f"{len(fwd)} forward PWM lines")
car.close()

# R5 RESET in open space: released, car waits for the operator ----------------
car = new_car()
car.cs.send_speed_command(200)
js.settle(0.3)
car.sim("dist 20")
car.wait(r"OBSTACLE_DETECTED", timeout=2)
js.settle()
car.sim("dist 0")
time.sleep(0.5)
m = car.mark()
route(car, "obstacleReset")
clr = car.wait(r"OBSTACLE_CLEARED", since=m, timeout=1)
js.settle(0.3)
still = car.pwm
route(car, "changeSpeed", {"speed": "200", "direction": "forward"})
js.settle(0.3)
report("R5 RESET in open space: released, no auto-resume, operator speed moves",
       clr is not None and still == 0 and car.pwm == js.duty(200), f"cleared={clr is not None} still={still} after={car.pwm}")
car.close()

# R6 EMERGENCY BRAKE during a stop-sign wait cancels the scheduled resume ------
car, det, sc = with_signs()
with quiet:
    det.dets = [STOP_SIGN]
    time.sleep(0.5)
    route(car, "brake")
    time.sleep(3.5)
report("R6 stop sign, BRAKE pressed during the wait: car stays stopped", car.pwm == 0,
       f"pwm={car.pwm} pending={sc.pending_resume_at is not None}")
with quiet:
    sc.stop()
car.close()

# R7 STOP (speed 0) from the dashboard also cancels --------------------------
car, det, sc = with_signs()
with quiet:
    det.dets = [STOP_SIGN]
    time.sleep(0.5)
    route(car, "changeSpeed", {"speed": "0"})
    time.sleep(3.5)
report("R7 stop sign, dashboard speed 0 during the wait: car stays stopped", car.pwm == 0, f"pwm={car.pwm}")
with quiet:
    sc.stop()
car.close()

# R8 DISARM during the wait, re-arm, obstacle passes ---------------------------
car, det, sc = with_signs()
with quiet:
    det.dets = [STOP_SIGN]
    time.sleep(0.5)
    route(car, "disarm")
    time.sleep(3.5)
    route(car, "arm")
    time.sleep(0.3)
    p = car.pwm
    obstacle_passes(car)
report("R8 DISARM during stop wait -> re-arm -> obstacle passes: still", p == 0 and car.pwm == 0,
       f"after arm={p} after obstacle={car.pwm} last_speed={car.cs.last_speed}")
with quiet:
    sc.stop()
car.close()

# R9 stop sign without interference still resumes -----------------------------
car, det, sc = with_signs()
with quiet:
    det.dets = [STOP_SIGN]
    time.sleep(0.4)
    p0 = car.pwm
    time.sleep(sc.strategies["stop"].stop_duration + 0.8)
report("R9 stop sign alone: stops, then resumes (unchanged)", p0 == 0 and car.pwm > 0, f"{p0} -> {car.pwm}")
with quiet:
    sc.stop()
car.close()

# R10 increase-speed strategy: its delayed reset respects a brake --------------
car, det, sc = with_signs()
with quiet:
    det.dets = [{"class": "one-way", "confidence": 0.9, "distance": 0.5}]
    time.sleep(0.4)
    fast = car.pwm
    route(car, "brake")
    time.sleep(3.5)
report("R10 one-way sign then BRAKE: the 3 s speed reset does not restart the car",
       fast > 0 and car.pwm == 0, f"before brake={fast} after 3.5 s={car.pwm}")
with quiet:
    sc.stop()
car.close()

# R11 parking: obstacle during FORWARD_LEFT aborts the maneuver ----------------
car, det, sc = with_signs()
park = sc.strategies["park"]
park.wait_target_distance_cm = 1  # skip WAIT
with quiet:
    det.dets = [{"class": "park", "confidence": 0.9, "distance": 1.0}]
    t0 = time.time()
    while park.phase != "FORWARD_LEFT" and time.time() - t0 < 3:
        time.sleep(0.01)
    m = car.mark()
    car.sim("dist 20")
    t0 = time.time()
    while park.is_running and time.time() - t0 < 3:
        time.sleep(0.02)
    js.settle(0.5)
back = car.seen(r"#HW motor dir=B pwm=[1-9]", m)
report("R11 parking: obstacle in FORWARD_LEFT aborts, no reverse arcs",
       not park.is_running and not back and car.pwm == 0, f"running={park.is_running} reverse lines={len(back)} pwm={car.pwm}")
with quiet:
    sc.stop()
car.close()

# R12 parking without obstacles still completes --------------------------------
car, det, sc = with_signs()
park = sc.strategies["park"]
park.wait_target_distance_cm = 1
for attr in ("forward_left_duration", "back_right_duration", "back_left_duration"):
    setattr(park, attr, 0.4)  # shorter run, same phases
with quiet:
    det.dets = [{"class": "park", "confidence": 0.9, "distance": 1.0}]
    t0 = time.time()
    while (not park.is_parked) and time.time() - t0 < 10:
        time.sleep(0.05)
    js.settle(0.2)
report("R12 parking without obstacles completes, no plugging", park.is_parked and not car.violations and car.pwm == 0,
       f"parked={park.is_parked} violations={car.violations} pwm={car.pwm}")
with quiet:
    sc.stop()
car.close()

# R13 reboot mid-session with the DISARMED line lost --------------------------
car = RealCar(BIN)
orig = car.on_event
car.on_event = lambda ev: None if ev.startswith("EVENT:STATE_CHANGED:DISARMED") else orig(ev)
car.g["system_state"]["state"] = "RUNNING"  # Jetson still believes RUNNING
car.ap.last_servo_angle = 90
car.sim("dist 200")
car.boot()
run_autopilot(car, 90)
report("R13 reboot, STATE_CHANGED:DISARMED lost: autopilot still re-sends its angle", car.servo == 90, f"servo={car.servo}")
car.close()

# R14 autopilot resume() re-sends the angle ---------------------------------
car = new_car()
run_autopilot(car, 80, 3)
car.cs.send_steering_command(105)  # someone else moved the servo while paused
js.settle()
with quiet:
    car.ap.resume()  # real AutoPilotController.resume()
run_autopilot(car, 80, 3)
report("R14 after resume the autopilot re-sends an unchanged angle", car.servo == 80, f"servo={car.servo}")
car.close()

fails = [r for r in results if r[1] is False]
print(f"\n{sum(1 for r in results if r[1])} pass, {len(fails)} fail")
sys.exit(1 if fails else 0)
