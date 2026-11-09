#!/usr/bin/env python3
"""Firmware regressions found by review, run on the host simulator.

Raw protocol driver (no Jetson code): each check is a bug that existed at some
point. Usage:

    python3 fw_regressions.py build/fw_sim [build/fw_wrap]

The second binary, built with -DSIM_TICK_OFFSET=4294964796u (2.5 s before the
32-bit tick wraps), enables the watchdog-at-wrap check.
"""
import re
import subprocess
import sys
import threading
import time

LINE_RE = re.compile(r"^t=(\d+) (.*)$")
BIN = sys.argv[1]
WRAP_BIN = sys.argv[2] if len(sys.argv) > 2 else None
results = []


def report(name, ok, detail):
    results.append((name, ok, detail))
    print(f"{'PASS' if ok else 'FAIL'}  {name}  ({detail})", flush=True)


class Fw:
    def __init__(self, binary, ping=True, pre=()):
        self.p = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.lines, self.lk, self.wl, self.ping = [], threading.Lock(), threading.Lock(), ping
        for c in pre:
            self.w(c)
        threading.Thread(target=self._rd, daemon=True).start()
        threading.Thread(target=self._hb, daemon=True).start()

    def w(self, *cmds):
        """Write several lines in ONE write: they arrive in the same poll."""
        with self.wl:
            try:
                self.p.stdin.write("".join(c + "\n" for c in cmds))
                self.p.stdin.flush()
            except BrokenPipeError:
                pass

    def _hb(self):
        while self.p.poll() is None:
            if self.ping:
                self.w("M:PING:0")
            time.sleep(0.05)

    def _rd(self):
        for raw in self.p.stdout:
            m = LINE_RE.match(raw.rstrip("\n"))
            if m:
                with self.lk:
                    self.lines.append((int(m.group(1)), m.group(2)))

    def mark(self):
        with self.lk:
            return len(self.lines)

    def since(self, m, pat):
        with self.lk:
            return [l for l in self.lines[m:] if re.search(pat, l[1])]

    def wait(self, pat, m=0, timeout=2.0):
        end = time.time() + timeout
        while time.time() < end:
            r = self.since(m, pat)
            if r:
                return r[0]
            time.sleep(0.005)
        return None

    def pwm(self):
        hw = self.since(0, r"#HW motor")
        return int(hw[-1][1].split("pwm=")[1]) if hw else 0

    def close(self):
        self.w("#SIM quit")
        try:
            self.p.wait(1)
        except Exception:
            self.p.kill()


def boot(binary=BIN, **kw):
    f = Fw(binary, **kw)
    f.wait(r"SYSTEM_READY", timeout=3)
    f.w("#SIM dist 200", "M:SYS_ARM:0")
    f.wait(r"STATE_CHANGED:RUNNING", timeout=2)
    return f


def deadtime_check(f, m):
    bad = f.since(m, r"VIOLATION")
    dts = [int(l.split("=")[1]) for _, l in f.since(m, r"#HW reverse deadtime")]
    return (not bad and dts and min(dts) >= 60), f"deadtimes={dts} violations={len(bad)}"


S = 0.25

# 1. Dead time through a braked state ---------------------------------------
for name, stop in [("SET_SPEED:0", ("C:SET_SPEED:0",)), ("E:BRAKE_NOW", ("E:BRAKE_NOW",))]:
    f = boot()
    f.w("C:SET_DIR:1", "C:SET_SPEED:200")
    time.sleep(S)
    m = f.mark()
    f.w(*stop)
    time.sleep(0.02)
    f.w("C:SET_DIR:0", "C:SET_SPEED:200")
    time.sleep(S)
    ok, d = deadtime_check(f, m)
    report(f"F1 forward 200 -> {name} -> reverse 20 ms later: still waits 60 ms", ok and f.pwm() == 200, d)
    f.close()

f = boot()
f.w("C:SET_DIR:1", "C:SET_SPEED:200")
time.sleep(S)
m = f.mark()
f.w("#SIM dist 20")
f.wait(r"OBSTACLE_DETECTED", m)
f.w("C:SET_DIR:0", "C:SET_SPEED:150")  # back away right away
time.sleep(S)
ok, d = deadtime_check(f, m)
report("F1 obstacle -> Jetson backs away at once: still waits 60 ms", ok and f.pwm() == 150, d)
f.close()

f = boot()
f.w("C:SET_DIR:1", "C:SET_SPEED:200")
time.sleep(S)
m = f.mark()
f.w("C:SET_DIR:0", "C:SET_SPEED:200")
time.sleep(S)
ok, d = deadtime_check(f, m)
report("F1 direct reverse with speed applied: 60 ms", ok, d)
f.close()

f = boot()
f.w("C:SET_DIR:1", "C:SET_SPEED:200")
time.sleep(S)
f.w("C:SET_SPEED:0")
time.sleep(0.3)
m = f.mark()
f.w("C:SET_DIR:0", "C:SET_SPEED:200")
time.sleep(0.05)
on = f.since(m, r"#HW motor dir=B pwm=200")
report("F1 reverse long after the stop: no extra wait", bool(on) and on[0][0] - f.since(m, r".")[0][0] < 30,
       f"{len(on)} lines")
f.close()

# 2. Brake vs speed in the same poll: order of arrival decides --------------
bad = 0
for _ in range(10):
    f = boot()
    f.w("C:SET_SPEED:100")
    time.sleep(S)
    f.w("C:SET_SPEED:200", "E:BRAKE_NOW")
    time.sleep(S)
    bad += f.pwm() != 0
    f.close()
report("F2 SET_SPEED then BRAKE_NOW in one write ends braked (10 tries)", bad == 0, f"{bad} ended moving")
f = boot()
m = f.mark()
f.w("E:BRAKE_NOW", "C:SET_SPEED:120")
time.sleep(S)
report("F2 BRAKE_NOW then SET_SPEED in one write ends moving", f.pwm() == 120 and bool(f.since(m, r"CMD_EXECUTED:BRAKE_NOW")),
       f"pwm={f.pwm()}")
f.close()

# 3. DISARM + ARM back to back --------------------------------------------
bad = 0
for _ in range(3):
    f = boot()
    f.w("C:SET_SPEED:200")
    time.sleep(0.2)
    m = f.mark()
    f.w("M:SYS_DISARM:0", "M:SYS_ARM:0")
    time.sleep(0.3)
    seen = bool(f.since(m, r"STATE_CHANGED:DISARMED"))
    bad += f.pwm() != 0 or not seen
    f.close()
report("F3 DISARM then ARM in one write while driving: disarm runs, car ends still", bad == 0, f"{bad}/3 bad")
f = boot()
m = f.mark()
f.w("M:SYS_ARM:0", "M:SYS_DISARM:0")
time.sleep(0.2)
ev = [l for _, l in f.since(m, r"EVENT:(CMD_EXECUTED:SYS_|STATE_CHANGED)")]
states = [e for e in ev if "STATE_CHANGED" in e]
report("F3 ARM then DISARM: both confirmed, ends DISARMED",
       "USB EVENT:CMD_EXECUTED:SYS_ARM" in ev and "USB EVENT:CMD_EXECUTED:SYS_DISARM" in ev
       and bool(states) and states[-1].endswith("DISARMED") and f.pwm() == 0,
       str([e[4:] for e in ev if e.startswith("USB")]))
f.close()

# 4. Confirmations -----------------------------------------------------------
f = Fw(BIN)
f.wait(r"SYSTEM_READY", timeout=3)
m = f.mark()
for c in ("M:SYS_DISARM:0", "M:SYS_STOP:0", "M:SYS_STOP:0", "M:SYS_RESUME:0", "M:SYS_RESUME:0"):
    f.w(c)
    time.sleep(0.05)
got = [l[4:] for _, l in f.since(m, r"USB EVENT:CMD_EXECUTED")]
report("F4 DISARM while disarmed, STOP x2, RESUME x2: all confirmed", got == [
    "EVENT:CMD_EXECUTED:SYS_DISARM", "EVENT:CMD_EXECUTED:SYS_STOP", "EVENT:CMD_EXECUTED:SYS_STOP",
    "EVENT:CMD_EXECUTED:SYS_RESUME", "EVENT:CMD_EXECUTED:SYS_RESUME"], str(got))
f.close()

# 5. Rejected SET_SPEED reaches the Jetson (UART too) -------------------------
f = boot()
m = f.mark()
f.w("M:SYS_STOP:0")
time.sleep(0.05)
f.w("C:SET_SPEED:200")
time.sleep(0.05)
f.w("C:SET_SPEED:200")
time.sleep(0.1)
rej = f.since(m, r"UART EVENT:CMD_REJECTED:SET_SPEED:SYS_STOP")
report("F5 SET_SPEED during SYS_STOP -> one CMD_REJECTED:SET_SPEED:SYS_STOP on UART", len(rej) == 1, f"{len(rej)} events")
f.w("M:SYS_RESUME:0", "#SIM dist 20")
f.wait(r"OBSTACLE_DETECTED", m)
f.w("C:SET_SPEED:200")
time.sleep(0.1)
report("F5 forward SET_SPEED against an obstacle -> held for resume (OBSTACLE_HOLD:200), not rejected",
       bool(f.since(m, r"EVENT:OBSTACLE_HOLD:200")) and not f.since(m, r"CMD_REJECTED:SET_SPEED:OBSTACLE"), "")
f.close()

# 6. Watchdog across the 32-bit tick wrap -------------------------------------
if WRAP_BIN:
    f = boot(WRAP_BIN)
    f.w("C:SET_SPEED:200")
    # Cut the link ~200 ms before the 32-bit tick wraps: the 300 ms timeout
    # then falls AFTER the wrap, where "now < heartbeat" used to read as age 0.
    target = 2**32 - 200
    while True:
        m = f.mark()
        f.w("#SIM now")
        line = f.wait(r"#SIM now=", m, timeout=1)
        now = int(line[1].split("=")[1]) if line else 0
        if now >= target or now < 2**31:  # reached, or already wrapped (too late)
            break
        time.sleep(min(0.05, (target - now) / 1000))
    wrapped_before_cut = now < 2**31
    m = f.mark()
    f.ping = False
    wd = f.wait(r"WATCHDOG_TIMEOUT", m, timeout=2)
    time.sleep(0.1)
    report("F6 link lost ~200 ms before the tick wraps -> watchdog still fires",
           not wrapped_before_cut and wd is not None and f.pwm() == 0,
           f"cut at t={now} timeout={'yes' if wd else 'no'} pwm={f.pwm()}")
    f.close()

# 7. Watchdog FAULT recovers by itself; E-STOP FAULT does not ------------------
f = boot()
f.w("C:SET_SPEED:200")
time.sleep(0.1)
f.ping = False
m = f.mark()
f.wait(r"WATCHDOG_TIMEOUT", m, timeout=2)
f.w("M:PING:0")  # one stray command, then silence: not stable
time.sleep(1.0)
report("F7 one command after a watchdog FAULT: no recovery", not f.since(m, r"WATCHDOG_RECOVERED"), "")
m = f.mark()
f.ping = True
f.w("C:SET_SPEED:150")  # sent during the FAULT: must not survive the recovery
t0 = time.time()
rec = f.wait(r"USB EVENT:WATCHDOG_RECOVERED", m, timeout=2)
st = f.wait(r"USB EVENT:STATE_CHANGED:RUNNING", m, timeout=2)
dt = time.time() - t0
time.sleep(0.1)
report("F7 heartbeat back for ~500 ms -> WATCHDOG_RECOVERED, RUNNING, speed 0",
       rec is not None and st is not None and rec[0] <= st[0] and 0.45 <= dt <= 0.8 and f.pwm() == 0,
       f"after {dt * 1000:.0f} ms pwm={f.pwm()}")
f.w("C:SET_SPEED:120")
time.sleep(0.1)
report("F7 after recovery a new SET_SPEED moves", f.pwm() == 120, f"pwm={f.pwm()}")
f.close()

f = boot()
m = f.mark()
f.w("#SIM estop 1")
f.wait(r"STATE_CHANGED:FAULT", m)
f.w("#SIM estop 0")
time.sleep(1.0)
report("F7 E-STOP FAULT with the heartbeat alive: stays FAULT",
       not f.since(m, r"WATCHDOG_RECOVERED|STATE_CHANGED:RUNNING"), "")
f.close()

f = boot()
f.ping = False
m = f.mark()
f.wait(r"WATCHDOG_TIMEOUT", m, timeout=2)
f.ping = True
time.sleep(0.2)
f.w("#SIM estop 1")  # E-STOP during the watchdog FAULT takes over
time.sleep(0.05)
f.w("#SIM estop 0")
time.sleep(1.0)
report("F7 E-STOP during a watchdog FAULT: no recovery", not f.since(m, r"WATCHDOG_RECOVERED"), "")
f.close()

# 8. CMD_REJECTED:SET_SPEED for every different speed, rate limited ----------
f = boot()
f.w("M:SYS_STOP:0")
time.sleep(0.1)
m = f.mark()
for v in (100, 150, 150):  # separate cycles: one write would keep only the last
    f.w(f"C:SET_SPEED:{v}")
    time.sleep(0.03)
time.sleep(0.4)
rej = f.since(m, r"USB EVENT:CMD_REJECTED:SET_SPEED:SYS_STOP")
gap = rej[1][0] - rej[0][0] if len(rej) == 2 else 0
report("F8 100 then 150 twice during SYS_STOP: 2 rejections, >= 200 ms apart", len(rej) == 2 and gap >= 200,
       f"{len(rej)} events gap={gap} ms")
m = f.mark()
f.w("C:SET_SPEED:200")
time.sleep(0.1)
report("F8 a new speed after the interval is rejected again",
       len(f.since(m, r"USB EVENT:CMD_REJECTED:SET_SPEED")) == 1, "")
f.close()

# 9. EVENT:STOPPED_BY ---------------------------------------------------------
f = boot()
m = f.mark()
f.w("M:SYS_STOP:0")
f.wait(r"STATE_CHANGED:STOPPED", m)
by = f.wait(r"USB EVENT:STOPPED_BY:", m)
report("F9 SYS_STOP -> STOPPED_BY:SYS_STOP", by is not None and by[1].endswith("SYS_STOP"), str(by))
m = f.mark()
f.w("#SIM dist 20")
f.wait(r"OBSTACLE_DETECTED", m)
time.sleep(0.05)
f.w("M:SYS_RESUME:0")
by = f.wait(r"USB EVENT:STOPPED_BY:OBSTACLE", m)
report("F9 obstacle under SYS_STOP, then RESUME -> STOPPED_BY:OBSTACLE", by is not None, "")
m = f.mark()
f.w("M:SYS_ARM:0")
time.sleep(0.1)
report("F9 SYS_ARM while STOPPED repeats STOPPED_BY", bool(f.since(m, r"USB EVENT:STOPPED_BY:OBSTACLE")), "")
f.close()

f = Fw(BIN)
f.wait(r"SYSTEM_READY", timeout=3)
f.w("#SIM dist 200", "M:SYS_STOP:0")
time.sleep(0.05)
m = f.mark()
f.w("M:SYS_ARM:0")
f.wait(r"CMD_EXECUTED:SYS_ARM", m)
ev = [l[4:] for _, l in f.since(m, r"USB EVENT:(STATE_CHANGED|STOPPED_BY)")]
report("F9 arming into STOPPED reports STOPPED_BY:SYS_STOP once",
       ev == ["EVENT:STATE_CHANGED:STOPPED", "EVENT:STOPPED_BY:SYS_STOP"], str(ev))
f.close()

# 10. M:STATUS ----------------------------------------------------------------
f = boot()
f.w("C:SET_SPEED:120")
time.sleep(0.1)
m = f.mark()
f.w("M:STATUS:0")
st = f.wait(r"UART EVENT:STATUS:", m)
report("F10 M:STATUS while driving",
       st is not None and st[1] == "UART EVENT:STATUS:RUNNING:SYS_STOP=0:OBSTACLE=0:SPEED=120:HELD=0:ULTRASONIC=OK", str(st))
f.w("#SIM dist 20")
f.wait(r"OBSTACLE_HOLD", m)
time.sleep(0.05)
m = f.mark()
f.w("M:STATUS:0")
st = f.wait(r"USB EVENT:STATUS:", m)
report("F10 M:STATUS in an obstacle hold shows the speed it will resume to",
       st is not None and st[1] == "USB EVENT:STATUS:STOPPED:SYS_STOP=0:OBSTACLE=1:SPEED=0:HELD=120:ULTRASONIC=OK", str(st))
f.w("M:SYS_STOP:0")
time.sleep(0.05)
m = f.mark()
f.w("M:STATUS:0")
st = f.wait(r"USB EVENT:STATUS:", m)
report("F10 M:STATUS with SYS_STOP and an obstacle (SYS_STOP drops the held speed)",
       st is not None and st[1] == "USB EVENT:STATUS:STOPPED:SYS_STOP=1:OBSTACLE=1:SPEED=0:HELD=0:ULTRASONIC=OK", str(st))
f.close()

fails = [r for r in results if not r[1]]
print(f"\n{len(results) - len(fails)} pass, {len(fails)} fail")
sys.exit(1 if fails else 0)
