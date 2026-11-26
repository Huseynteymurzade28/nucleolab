"""Press the blue button while this runs (10 s)."""
import glob, os, time, serial
p = serial.Serial(os.path.realpath(glob.glob("/dev/serial/by-id/*STLink*")[0]), 921600, timeout=1)
p.write(b"\n"); time.sleep(.1); p.reset_input_buffer()
r = []
for _ in range(200):
    p.write(b"DIG:INP?\n"); r.append(p.readline().strip().decode()); time.sleep(.05)
print("".join(r)); print("press seen:", "1" in r)
