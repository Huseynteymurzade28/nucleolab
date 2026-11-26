"""Remote-control demo through the bridge (start host/server.py first)."""
import pyvisa
i = pyvisa.ResourceManager("@py").open_resource("TCPIP::127.0.0.1::5025::SOCKET")
i.read_termination = i.write_termination = "\n"; i.timeout = 25000
print(i.query("*IDN?"))
i.write("*CLS;:SOUR:FUNC SIN;:SOUR:VOLT 2;:OUTP ON;:ACQ:SRAT 1E6;POIN 8192")
for f in (500, 1000, 5000, 20000):
    i.write(f"SOUR:FREQ {f}")
    print(f"{f:6d} Hz -> {float(i.query('MEAS:FREQ?')):10.2f} Hz   {float(i.query('MEAS:VOLT:PTP?')):.3f} Vpp")
i.write("MAKE:COFFEE"); print("MAKE:COFFEE ->", i.query("SYST:ERR?"))
i.write("SOUR:FREQ 1000;:ACQ:POIN 2048")
