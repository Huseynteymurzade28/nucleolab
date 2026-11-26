import sys, time, numpy as np
from lab import Lab
L = Lab(); bad = 0
def ok(name, cond, info=""):
    global bad; bad += not cond; print("OK  " if cond else "FAIL", name, info)
print(L.q("*IDN?")); ok("*TST?", L.q("*TST?") == "0")
print("VDDA", L.q("SYST:VDDA?"), "temp", L.q("MEAS:TEMP?"), "cal", L.q("DIAG:CAL?"))
jp, internal = L.f("DIAG:LOOP?")
ok("internal DAC->ADC", internal < .03, f"worst {internal*1e3:.1f} mV")
jumper = jp < .03; print(f"JUMPER PA4->PA0: {'present' if jumper else 'NOT detected'} (worst {jp*1e3:.0f} mV)")
L.w(f"ACQ:CHAN {1 if jumper else 3};:OUTP ON")
for f in (10, 100, 1000, 12345, 50000, 100000):
    sr = min(1e6, max(1e3, f * 200)); L.w(f"SOUR:FREQ {f};:ACQ:SRAT {sr};POIN 8192")
    a, m = L.f("SOUR:FREQ?")[0], L.f("MEAS:FREQ?")[0]
    ok(f"freq {f} Hz", abs(a/f-1) < 2e-3 and abs(m/a-1) < 2e-3, f"set actual {a:.2f}, measured {m:.2f}")
L.w("SOUR:FREQ 1000;:ACQ:SRAT 1e6;POIN 8000")
for fn, k in (("SIN", 8**-.5), ("SQU", .5), ("TRI", 12**-.5), ("RAMP", 12**-.5)):
    L.w(f"SOUR:FUNC {fn}"); ac, pp, dc = (L.f(f"MEAS:VOLT:{x}?")[0] for x in ("AC", "PTP", "DC"))
    ok(fn, abs(ac-2*k) < .02 and abs(pp-2) < .06 and abs(dc-1.65) < .02, f"AC {ac:.4f} (ideal {2*k:.4f}) PP {pp:.3f} DC {dc:.3f}")
L.w("SOUR:FUNC SIN;:TRIG:LEV 1.65"); L.w("ACQ:POIN 1024")
for sl, sg in (("POS", 1), ("NEG", -1)):
    L.w(f"TRIG:SLOP {sl}"); v = []
    for _ in range(30):
        t, y, tr = L.capture(); i = int(np.argmin(abs(t))); v.append(y[i]); assert tr
        good = (y[i+20]-y[i-20])*sg > 0
        if not good: break
    ok("trigger "+sl, good and max(abs(np.array(v)-1.65)) < .04, f"level {min(v):.3f}..{max(v):.3f}")
L.w("TRIG:SWE NORM;LEV 3.2;:INIT"); time.sleep(.3); ok("NORM keeps waiting", L.q("ACQ:STAT?") == "ARM")
L.w("TRIG:FORC"); L.q("*OPC?"); ok("force", L.q("ACQ:STAT?") == "DONE"); L.w("TRIG:SWE AUTO;LEV 1.65;SLOP POS")
x = np.arange(4096)/4096; arb = np.round(2047.5+1800*np.sin(2*np.pi*x)*np.sin(6*np.pi*x)).astype(int)
L.arb(arb); L.w("SOUR:FUNC ARB"); ok("ARB upload", L.q("SOUR:DATA:ARB:POIN?") == "4096" and L.err().startswith("0"))
L.w("SOUR:FUNC SIN;VOLT 3.6"); ok("conflict", L.err().startswith("-221"))
L.w("ACQ:POIN 1024"); t0 = time.time()
for _ in range(50): L.capture()
print(f"frame rate (1024 pts): {50/(time.time()-t0):.1f} fps")
L.w("SOUR:FREQ 1000;:ACQ:POIN 8192"); t, y, _ = L.capture()
f0 = L.f("SOUR:FREQ?")[0]; tt = t; A = np.column_stack([np.sin(2*np.pi*f0*tt), np.cos(2*np.pi*f0*tt), np.ones_like(tt)])
r = y - A @ np.linalg.lstsq(A, y, rcond=None)[0]; amp = np.hypot(*np.linalg.lstsq(A, y, rcond=None)[0][:2])
print(f"1 kHz sine SINAD {20*np.log10(amp/2**.5/r.std()):.1f} dB")
ok("no errors left", L.err().startswith("0"), "")
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
L.w("ACQ:POIN 4096"); fig, ax = plt.subplots(2, 2, figsize=(11, 6))
for a, fn in zip(ax.flat, ("SIN", "SQU", "TRI", "ARB")):
    L.w(f"SOUR:FUNC {fn}"); t, y, _ = L.capture(); a.plot(t*1e3, y, lw=1); a.set(title=fn, xlabel="ms", ylabel="V", ylim=(0, 3.3)); a.grid(alpha=.3)
fig.tight_layout(); fig.savefig("build/first_capture.png", dpi=90)
L.w("*RST"); print("RESULT", "PASS" if not bad else f"{bad} FAIL"); sys.exit(bool(bad))
