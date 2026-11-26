"""FFT checked against numpy; Bode null test on the internal channel (must read 0 dB, 0 deg)."""
import sys, numpy as np
from lab import Lab
L = Lab(); bad = 0
L.w("ACQ:CHAN 3;POIN 4096;SRAT 1e6;:SOUR:FREQ 10000;:OUTP ON"); t, y, _ = L.capture()
d = L.f("CALC:DIST?"); s = np.frombuffer(L.block("CALC:SPEC?"), "<i2")/100
i = np.arange(4096)*2*np.pi/4096; w = .35875-.48829*np.cos(i)+.14128*np.cos(2*i)-.01168*np.cos(3*i)
ref = 20*np.log10(np.abs(np.fft.rfft((y-y.mean())*w))[:2048]*2**.5/w.sum()+1e-9)
m = ref > ref.max()-70; e = np.abs(s-ref)[m].max(); bad += e > .2
print(f"{'OK' if e<=.2 else 'FAIL'} FFT vs numpy: worst {e:.3f} dB over {m.sum()} bins")
print("fund %.1f Hz  %.4f Vrms  THD %.1f dB  SNR %.1f dB  SINAD %.1f dB" % tuple(d[2:]))
bad += abs(d[2]-1e4) > 30 or abs(d[3]-.7071) > .01
L.w("SOUR:FREQ 1000;:ACQ:POIN 2048"); L.capture(); u = L.f("CALC:DIST?")[2]; bad += u < 1e37; print("OK  " if u > 1e37 else "FAIL", "unresolved tone reported as NaN")
for f in np.geomspace(10, 1e5, 25):
    r = L.f(f"BODE:POIN? {f:.3f},3"); ok = abs(r[1]) < .05 and abs(r[2]) < .5 and abs(r[3]-2) < .25
    bad += not ok; print("OK  " if ok else "FAIL", "%9.2f Hz %+.4f dB %+.3f deg ref %.3f Vpp" % tuple(r))
print(L.err()); print("RESULT", "PASS" if not bad else f"{bad} FAIL"); L.w("*RST"); sys.exit(bool(bad))
