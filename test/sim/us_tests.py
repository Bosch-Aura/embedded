#!/usr/bin/env python3
"""Ultrasonic scenarios on the host-simulated firmware (real ultrasonic_task.cpp
+ real hardware.cpp ultrasonic_read_cm on a simulated HC-SR04)."""
import sys
import time

import jetson_sim as js
from jetson_real import RealCar

BIN = sys.argv[1]
rows = []


def report(name, ok, detail):
    rows.append((name, ok, detail))
    print(f"{'PASS' if ok else 'INFO' if ok is None else 'FAIL'}  {name}  ({detail})", flush=True)


def car_running(speed=200):
    car = RealCar(BIN)
    car.sim("dist 200")
    car.boot()
    car.cs.send_speed_command(speed)
    js.settle(0.3)
    return car


def detections(car, since):
    return car.seen(r"USB EVENT:OBSTACLE_DETECTED", since), car.seen(r"USB EVENT:OBSTACLE_CLEARED", since)


# U1: threshold sweep -------------------------------------------------------
for d in [1, 2, 5, 10, 20, 29, 30, 31, 100, 399, 401]:
    car = car_running()
    m = car.mark()
    car.sim(f"dist {d}")
    time.sleep(1.0)
    det, _ = detections(car, m)
    expect = 2 <= d < 30
    report(f"U1 constant object at {d} cm -> {'brake' if expect else 'no brake'}",
           bool(det) == expect and (car.pwm == 0) == expect,
           f"detected={bool(det)} pwm={car.pwm}" + (" (sensor min range is 2 cm: closer reads as invalid)" if d < 2 else ""))
    car.close()

# U2: latency first close reading -> brake, and read period -------------------
car = car_running()
car.sim("uslog 1")
time.sleep(0.6)
reads = car.seen(r"#US raw=")
period_echo = (reads[-1][0] - reads[-6][0]) / 5 if len(reads) >= 6 else None
m = car.mark()
car.sim("dist 20")
first = car.wait(r"#US raw=20", since=m)
brake = car.wait(r"#HW motor .*pwm=0", since=m)
det = car.wait(r"USB EVENT:OBSTACLE_DETECTED", since=m)
lat = brake[0] - first[0] if first and brake else None
report("U2 latency: 1st close reading -> PWM 0", lat is not None and lat <= 200,
       f"{lat} ms (3 readings of debounce); read period with echo ~{period_echo:.0f} ms")
report("U2 brake within one cycle of OBSTACLE_DETECTED", det and brake and brake[0] - det[0] <= 10,
       f"{brake[0] - det[0] if det and brake else '?'} ms")
car.sim("dist 0")
time.sleep(1.0)
reads = car.seen(r"#US raw=noecho")
period_noecho = (reads[-1][0] - reads[-6][0]) / 5 if len(reads) >= 6 else None
report("U2 read period without echo", None, f"~{period_noecho:.0f} ms (50 ms + 30 ms pulseIn timeout)")
car.close()

# U3: HC-SR04 very close: valid one reading in four -------------------------
car = car_running()
m = car.mark()
car.sim("dropout 0.75")
car.sim("dist 6")
time.sleep(15.0)
det, clr = detections(car, m)
report("U3 object at 6 cm, 75% RANDOM loss for 15 s -> brake and hold", bool(det) and not clr and car.pwm == 0,
       f"detected={len(det)} cleared={len(clr)} pwm={car.pwm}")
car.close()

# U4: dropouts with a real obstacle at 20 cm ---------------------------------
# 90% is info only: 2 s of no-echo releases the brake (see U9/U16), and with
# 90% random loss that happens now and then.
for p in [0.5, 0.75, 0.9]:
    car = car_running()
    car.sim(f"dropout {p}")
    m = car.mark()
    car.sim("dist 20")
    time.sleep(15.0)
    det, clr = detections(car, m)
    report(f"U4 obstacle at 20 cm, {int(p*100)}% readings lost, 15 s -> stays stopped",
           (bool(det) and not clr) if p < 0.9 else None,
           f"detected={len(det)} cleared={len(clr)} pwm_now={car.pwm}")
    car.close()

# U5: object at the threshold edge with noise --------------------------------
for d, n in [(30, 3), (32, 4), (27, 5)]:
    car = car_running()
    car.sim(f"noise {n}")
    m = car.mark()
    car.sim(f"dist {d}")
    time.sleep(5.0)
    det, clr = detections(car, m)
    report(f"U5 object at {d}±{n} cm for 5 s -> no flapping", len(clr) <= 1,
           f"detected={len(det)} cleared={len(clr)}")
    car.close()

# U6: spurious close readings in open space ---------------------------------
for pat, expect in [("200,200,15,200,200", False), ("200,15,15,200", False), ("200,15,15,15,200,200,200,200", True)]:
    car = car_running()
    m = car.mark()
    car.sim(f"pattern {pat}")
    time.sleep(2.0)
    det, _ = detections(car, m)
    report(f"U6 pattern {pat} -> {'brake' if expect else 'ignored'}", bool(det) == expect, f"detections={len(det)}")
    car.close()

# U7: approaching object at ~0.75 m/s ----------------------------------------
car = car_running()
m = car.mark()
approach = list(range(150, 9, -4)) + [10] * 200
car.sim("pattern " + ",".join(map(str, approach)))
det = car.wait(r"USB EVENT:OBSTACLE_DETECTED", since=m, timeout=5)
report("U7 object closing at ~0.75 m/s: distance reported at brake", det is not None and int(det[1].split(":")[-1]) >= 15,
       det[1] if det else "never detected")
car.close()

# U8: dead / disconnected sensor ---------------------------------------------
car = car_running()
m = car.mark()
car.sim("dist 0")
time.sleep(3.0)
det, _ = detections(car, m)
warn = car.seen(r"ULTRASONIC|SENSOR", m)
report("U8 sensor disconnected (no echo ever) -> errors ignored, no brake", not det and car.pwm > 0,
       f"detections={len(det)} pwm={car.pwm} (by design; U17 covers the ULTRASONIC_SILENT warning)")
car.close()

# U9: clearing time ----------------------------------------------------------
for after, label in [(100, "object removed, wall at 1 m"), (0, "object removed, open space (no echo)"),
                     (-1, "sensor dies while blocked (no rising edge)")]:
    car = car_running()
    car.sim("dist 20")
    car.wait(r"OBSTACLE_DETECTED", timeout=2)
    js.settle(0.2)
    m = car.mark()
    car.sim(f"dist {after}")
    t_set = car.now()
    clr = car.wait(r"USB EVENT:OBSTACLE_CLEARED", since=m, timeout=4)
    js.settle(0.2)
    if after > 0:
        report(f"U9 {label}: clears and Jetson resumes", clr is not None and car.pwm > 0,
               f"cleared after ~{clr[0] - t_set if clr else '?'} ms, pwm={car.pwm}")
    elif after == 0:
        lag = clr[0] - t_set if clr else None
        report(f"U9 {label}: 2 s of no echo clears it", lag is not None and 1900 <= lag <= 2600,
               f"cleared after ~{lag} ms, pwm={car.pwm}")
    else:
        report(f"U9 {label}: a dead sensor is not evidence -> stays stopped", clr is None and car.pwm == 0,
               f"cleared={clr is not None} pwm={car.pwm}")
    car.close()

# U11: valid readings count even with errors in between --------------------
for pat, start_blocked, expect_change, label in [
    ("20,0,0,20,0,20,0,0,0", False, True, "near,err,err,near,err,near -> brake"),
    ("20,0,100,0,20,20", False, False, "near,err,FAR,err,near,near -> far restarts count, no brake"),
    ("100,0,0,100,0,0,0,100,0", True, True, "blocked: far,err,err,far,err,far -> clear"),
    ("100,100,20,100,100", True, False, "blocked: far,far,NEAR,far,far -> near restarts count, stays"),
]:
    car = car_running()
    if start_blocked:
        car.sim("dist 20")
        car.wait(r"OBSTACLE_DETECTED", timeout=2)
        js.settle(0.2)
    m = car.mark()
    # play the pattern ONCE, then hold no-echo
    car.sim("pattern " + pat + "," + ",".join(["0"] * 200))
    time.sleep(2.0)
    det, clr = detections(car, m)
    changed = bool(clr) if start_blocked else bool(det)
    report(f"U11 {label}", changed == expect_change, f"detected={len(det)} cleared={len(clr)}")
    car.close()

# U10: reverse away from an obstacle ahead ------------------------------------
car = car_running()
car.sim("dist 15")
car.wait(r"OBSTACLE_DETECTED", timeout=2)
js.settle(0.2)
car.cs.send_speed_command(150, "backward")
js.settle(0.5)
report("U10 obstacle ahead, Jetson asks to back up -> backs away", car.pwm == js.duty(150) and car.dir == "B",
       f"pwm={car.pwm} dir={car.dir}")
car.cs.send_speed_command(150, "forward")  # change of mind: forward again
js.settle(0.3)
report("U10 ...then forward again -> blocked, speed cleared", car.pwm == 0, f"pwm={car.pwm} dir={car.dir}")
car.close()

# U12: obstacle appears while already reversing -> keeps reversing
car = car_running()
car.cs.send_speed_command(150, "backward")
js.settle(0.3)
m = car.mark()
car.sim("dist 15")
car.wait(r"OBSTACLE_DETECTED", since=m, timeout=2)
js.settle(0.3)
report("U12 obstacle ahead while reversing -> keeps backing away", car.pwm == js.duty(150) and car.dir == "B",
       f"pwm={car.pwm} dir={car.dir}")
car.close()

# U13: SYS_STOP still blocks everything (reverse and servo)
car = car_running()
car.raw("M:SYS_STOP:0")
js.settle()
car.cs.send_speed_command(150, "backward")
car.cs.send_steering_command(70)
js.settle(0.3)
report("U13 SYS_STOP: reverse blocked and servo frozen", car.pwm == 0 and car.servo == 105, f"pwm={car.pwm} servo={car.servo}")
car.close()

# U14: stale evidence -> no false stop
car = car_running()
m = car.mark()
car.sim("pattern " + ",".join(["15"] + ["0"] * 37))  # 1 spurious near reading every ~3 s
time.sleep(11)
det, _ = detections(car, m)
report("U14 1 spurious near reading every ~3 s in open space, 11 s -> no stop", not det and car.pwm > 0,
       f"detections={len(det)} pwm={car.pwm}")
car.close()

# U15: brake latency vs echo loss (evidence window must not break braking)
import statistics
for p in (0.0, 0.5, 0.75, 0.9):
    lats = []
    for _ in range(3):
        car = car_running()
        car.sim(f"dropout {p}")
        js.settle(0.2)
        t_set = time.time()
        m = car.mark()
        car.sim("dist 20")
        b = car.wait(r"#HW motor .*pwm=0", since=m, timeout=10)
        lats.append(round((time.time() - t_set) * 1000) if b else None)
        car.close()
    got = [x for x in lats if x]
    report(f"U15 obstacle at 20 cm, {int(p*100)}% loss -> brakes", len(got) == len(lats),
           f"latency ms {lats}, median {statistics.median(got) if got else '-'}")

# U16: OBSTACLE_RESET
car = car_running()
car.sim("dist 20")
car.wait(r"OBSTACLE_DETECTED", timeout=2)
js.settle(0.2)
car.sim("dist 0")  # obstacle removed sideways, nothing in range
time.sleep(0.8)    # before the 2 s no-echo release
stuck = car.pwm == 0 and car.state == "STOPPED"
m = car.mark()
car.cs.send_obstacle_reset()
clr = car.wait(r"USB EVENT:OBSTACLE_CLEARED", since=m, timeout=1)
js.settle(0.3)
held = car.pwm
car.cs.send_speed_command(200)  # the operator decides to go
js.settle(0.3)
report("U16 open space, OBSTACLE_RESET before the no-echo release: clears, car waits for the operator",
       stuck and clr is not None and held == 0 and car.pwm == js.duty(200),
       f"stuck={stuck} cleared={clr is not None} after reset={held} after operator speed={car.pwm}")
m = car.mark()
car.sim("dist 20")
car.cs.send_obstacle_reset()
js.settle(0.1)
again = car.wait(r"USB EVENT:OBSTACLE_DETECTED", since=m, timeout=2)
js.settle(0.1)
report("U16 OBSTACLE_RESET with the obstacle still there -> blocks again", again is not None and car.pwm == 0,
       f"re-detected after {again[0] - car.seen('OBSTACLE_RESET', m)[0][0] if again and car.seen('OBSTACLE_RESET', m) else '?'} ms, pwm={car.pwm}")
car.close()

# U17: sensor health events (informational)
car = car_running()
car.sim("uslog 1")
js.settle(0.2)
m = car.mark()
car.sim("dist 0")
first_silent = car.wait(r"#US raw=noecho", since=m, timeout=2)
sil = car.wait(r"USB EVENT:ULTRASONIC_SILENT", since=m, timeout=4)
last_valid = [l for l in car.seen(r"#US raw=\d", 0) if l[0] <= first_silent[0]][-1] if first_silent else None
gap = sil[0] - last_valid[0] if sil and last_valid else None
report("U17 no valid readings 2 s -> ULTRASONIC_SILENT, still driving", gap is not None and 1900 <= gap <= 2300 and car.pwm > 0,
       f"{gap} ms after the last valid reading, pwm={car.pwm}")
silent_flag = car.g["ultra_last_status"].get("sensor_silent")
m = car.mark()
car.sim("dist 100")
ok = car.wait(r"USB EVENT:ULTRASONIC_OK", since=m, timeout=2)
js.settle(0.1)
report("U17 valid reading again -> ULTRASONIC_OK; Jetson tracked both",
       ok is not None and silent_flag is True and car.g["ultra_last_status"]["sensor_silent"] is False,
       f"jetson sensor_silent {silent_flag} -> {car.g['ultra_last_status']['sensor_silent']}")
car.close()

fails = [r for r in rows if r[1] is False]
print(f"\n{sum(1 for r in rows if r[1])} pass, {len(fails)} fail, {sum(1 for r in rows if r[1] is None)} info")
sys.exit(1 if fails else 0)
