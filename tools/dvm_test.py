"""On-board test of the voltmeter: channels, statistics, limits, events,
plug detection. Needs nothing wired: with detection switched off every
input counts as connected, which is enough to exercise the rest.
Run with the bridge stopped (or it goes through the bridge if running)."""
import math, sys, time
from scpi import Link

L = Link(sys.argv[1] if len(sys.argv) > 1 else None)
bad = 0

def q(cmd): return ",".join(L.query(cmd))
def nums(cmd): return [float(x) for x in q(cmd).split(",")]
def check(name, ok, info=""):
    global bad
    bad += not ok
    print("OK  " if ok else "FAIL", f"{name:44} {info}")
def err(): return q("SYST:ERR?")

q("*RST;*CLS")
check("names", q("ROUT:CHAN:NAME?") == '"A0","A1","A2","A3","A4","A5"')
check("defaults", q("VOLT:AVER:COUN?;:CALC:LIM?;:ROUT:CHAN:DET?;DET:THR?") == "64;0;1;300")

# what is plugged in right now
conn = [int(x) for x in q("ROUT:CHAN:CONN?").split(",")]
v = nums("MEAS:VOLT? (@1:6)")
check("empty channels read NaN", all((c == 1) == (x < 1e30) for c, x in zip(conn, v)), f"conn {conn}")
cat = q("ROUT:CHAN:CAT?")
check("catalog matches", cat == "(@" + ",".join(str(i + 1) for i, c in enumerate(conn) if c) + ")", cat)

# everything below with detection off: all six channels connected
q("ROUT:CHAN:DET OFF")
check("detection off: all connected", q("ROUT:CHAN:CONN?") == "1,1,1,1,1,1")
v = nums("MEAS:VOLT? (@1:6)")
vdd = nums("SYST:VDDA?")[0]
check("VDDA plausible", 3.0 < vdd < 3.6, f"{vdd:.3f} V")
check("all readings in range", all(-0.01 <= x <= vdd + 0.01 for x in v), " ".join(f"{x:.3f}" for x in v))
raw, volt = nums("MEAS:VOLT:RAW? (@2)")[0], nums("MEAS:VOLT? (@2)")[0]
check("raw code matches volts", abs(raw * vdd / 4096 - volt) < 0.05, f"{raw:.0f} codes, {volt:.3f} V")

q("VOLT:AVER:COUN 1"); t0 = time.time(); [q("MEAS:VOLT? (@1:6)") for _ in range(20)]; fast = time.time() - t0
q("VOLT:AVER:COUN 256"); t0 = time.time(); [q("MEAS:VOLT? (@1:6)") for _ in range(20)]; slow = time.time() - t0
check("averaging count changes the work", slow > fast, f"1: {fast*50:.1f} ms, 256: {slow*50:.1f} ms per query")
q("VOLT:AVER:COUN 64")

# statistics fill in the background
q("CALC:AVER:CLE"); time.sleep(0.8)
n = [int(x) for x in q("CALC:AVER:COUN?").split(",")]
lo, hi, avg = nums("CALC:AVER:MIN?"), nums("CALC:AVER:MAX?"), nums("CALC:AVER?")
check("statistics collect samples", all(k >= 3 for k in n), f"counts {n}")
check("min <= mean <= max", all(a <= m + 1e-6 and m <= b + 1e-6 for a, m, b in zip(lo, avg, hi)))

# limits: an upper limit of 0 V puts every channel out of limits
while q("ROUT:CHAN:EVEN?") != "0,0,0": pass
q("CALC:LIM:LOW 0;UPP 0;:CALC:LIM ON"); time.sleep(0.5)
check("limit fail on all", q("CALC:LIM:FAIL?") == "1,1,1,1,1,1")
check("QUES bit 10 set", int(q("STAT:QUES:COND?")) & 1024 == 1024)
events = []
while (e := q("ROUT:CHAN:EVEN?")) != "0,0,0": events.append(e)
check("six limit events logged", len(events) == 6 and all(e.split(",")[1] == "2" for e in events), f"{len(events)} events")
q("CALC:LIM:UPP 3.6"); time.sleep(0.5)
check("back within limits", q("CALC:LIM:FAIL?") == "0,0,0,0,0,0" and int(q("STAT:QUES:COND?")) & 1024 == 0)
ms = [int(e.split(",")[2]) for e in events]
check("event times increase", ms == sorted(ms), f"{ms[0]}..{ms[-1]} ms")
q("CALC:LIM OFF")

# detection back on: the empty inputs are found again
while q("ROUT:CHAN:EVEN?") != "0,0,0": pass
q("ROUT:CHAN:DET ON"); time.sleep(0.6)
again = [int(x) for x in q("ROUT:CHAN:CONN?").split(",")]
check("detection on restores the state", again == conn, f"{again}")
unplugged = []
while (e := q("ROUT:CHAN:EVEN?")) != "0,0,0": unplugged.append(int(e.split(",")[0]))
check("unplug events for the empty inputs", sorted(unplugged) == [i + 1 for i, c in enumerate(conn) if not c], f"{unplugged}")

# bad input
for cmd, code in (("MEAS:VOLT? (@0)", -222), ("MEAS:VOLT? (@1,)", -104), ("CALC:LIM:UPP 5", -222),
                  ("VOLT:AVER:COUN 0", -222), ("ROUT:CHAN:DET:THR 9999", -222)):
    q("*CLS"); L.query(cmd)
    check(f"{cmd} -> {code}", err().startswith(str(code)))
q("*RST;*CLS")
check("no errors left", err().startswith("0,"))
print("RESULT:", "PASS" if not bad else f"FAIL ({bad})")
sys.exit(bad != 0)
