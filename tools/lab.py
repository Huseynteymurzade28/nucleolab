import glob, os, numpy as np, pyvisa
class Lab:
    def __init__(self, port=None):
        port = port or os.path.realpath(glob.glob("/dev/serial/by-id/*STLink*")[0])
        self.i = pyvisa.ResourceManager("@py").open_resource(f"ASRL{port}::INSTR")
        self.i.baud_rate, self.i.timeout = 921600, 25000
        self.i.read_termination = self.i.write_termination = "\n"
        self.w(""); self.w("*RST;*CLS"); self.q("*OPC?")
    def w(self, c): self.i.write(c)
    def q(self, c): return self.i.query(c)
    def f(self, c): return [float(x) for x in self.q(c).split(",")]
    def err(self): return self.q("SYST:ERR?")
    def block(self, c):
        self.w(c)
        rd = lambda n: self.i.read_bytes(n, break_on_termchar=False)
        h = rd(2); assert h[:1] == b"#", h
        n = int(rd(int(h[1:2]))); d = rd(n); assert rd(1) == b"\n"
        return d
    def arb(self, codes):
        d = np.asarray(codes, "<u2").tobytes(); n = str(len(d))
        self.i.write_raw(b"SOUR:DATA:ARB #%d%s" % (len(n), n.encode()) + d + b"\n")
    def capture(self):
        self.w("INIT"); assert self.q("*OPC?") == "1"
        n, dt, lsb, pre, trig = self.f("WAV:PRE?")
        y = np.frombuffer(self.block("WAV:DATA?"), "<u2") * lsb
        return (np.arange(int(n)) - pre) * dt, y, int(trig)
