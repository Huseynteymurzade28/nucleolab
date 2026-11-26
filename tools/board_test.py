"""Hardware acceptance test over the ST-LINK serial port."""
import glob, sys, time
import pyvisa

ports = glob.glob("/dev/serial/by-id/*STLink*") or sys.exit("ST-LINK serial port not found")
inst = pyvisa.ResourceManager("@py").open_resource(f"ASRL{sys.argv[1] if len(sys.argv) > 1 else __import__('os').path.realpath(ports[0])}::INSTR")
inst.baud_rate, inst.timeout = 921600, 2000
inst.read_termination = inst.write_termination = "\n"
inst.write("")            # terminate any partial line left in the parser
inst.write("*RST;*CLS")
bad = 0

def check(cmd, want):
    global bad
    try:
        got = inst.query(cmd)
    except Exception as e:
        got = f"<{type(e).__name__}: {e}>"
    ok = want(got) if callable(want) else got == want
    bad += not ok
    print("OK  " if ok else "FAIL", f"{cmd:34} -> {got}")

check("*IDN?", lambda r: r.startswith("HUSO,NucleoLab-L452,") and r.count(",") == 3)
check("*TST?", "0")
check("SYST:CLOC?", "80000000")
check("OUTP:LED ON;LED?", "1")
check("OUTPut:LED:STATe OFF;STATe?", "0")
check("DIG:INP?", "0")   # button must be released
inst.write("OUTP:LED BANANA")
check("SYST:ERR?", '-224,"Illegal parameter value"')
inst.write("MAKE:COFFEE")
check("SYST:ERR?", '-113,"Undefined header"')
check("SYST:ERR?", '0,"No error"')
t0, u0 = time.monotonic(), float(inst.query("SYST:UPT?"))
for _ in range(1000):
    if not inst.query("*IDN?").startswith("HUSO,"):
        bad += 1
dt, du = time.monotonic() - t0, float(inst.query("SYST:UPT?")) - u0
print(f"1000 x *IDN?: {dt:.2f} s, {1000/dt:.0f} queries/s; board clock measured {du:.2f} s")
check("SYST:ERR:COUN?", "0")
print("RESULT:", "PASS" if not bad else f"{bad} FAILURES")
sys.exit(bool(bad))
