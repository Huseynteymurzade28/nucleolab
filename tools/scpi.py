#!/usr/bin/env python3
"""Send SCPI commands to NucleoLab and print the replies.

    tools/scpi.py '*IDN?' 'MEAS:VOLT? (@1:6)'    one-shot
    tools/scpi.py                                 interactive prompt
    tools/scpi.py -e 'CALC:LIM:UPP 9'             also print SYST:ERR? after each command

Goes through the bridge (127.0.0.1:5025) when ./run.sh is running, otherwise
straight to the ST-LINK serial port. Each command is followed by
:SYST:SYNC? <n>, so commands without a reply don't leave us waiting.
"""
import argparse, glob, itertools, os, socket, sys, time

class Link:
    def __init__(self, port=None):
        self.seq = itertools.count(1)
        self.buf = b""
        try:
            self.sock = socket.create_connection(("127.0.0.1", 5025), timeout=0.3)
            self.sock.settimeout(10)
            self.via = "bridge 127.0.0.1:5025"
            self.ser = None
        except OSError:
            import serial
            path = port or next(iter(glob.glob("/dev/serial/by-id/*STLink*")), "/dev/ttyACM0")
            self.ser = serial.Serial(path, 921600, timeout=10, exclusive=True)
            self.ser.write(b"\n*CLS\n"); time.sleep(0.2); self.ser.reset_input_buffer()
            self.sock = None
            self.via = os.path.realpath(path)

    def _send(self, b): (self.sock.sendall if self.sock else self.ser.write)(b)
    def _line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096) if self.sock else self.ser.read(max(1, self.ser.in_waiting))
            if not chunk: raise TimeoutError("no reply from the board")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode(errors="replace").rstrip("\r")

    def query(self, cmd):
        """All reply lines of one command (none for a plain setting)."""
        n = next(self.seq)
        self._send(f"{cmd}\n:SYST:SYNC? {n}\n".encode())
        out = []
        while (line := self._line()) != f"SYNC,{n}":
            out.append(line)
        return out

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("commands", nargs="*")
    ap.add_argument("-e", "--errors", action="store_true", help="print SYST:ERR? after each command")
    ap.add_argument("-p", "--port", help="serial port (default: the ST-LINK)")
    a = ap.parse_args()
    link = Link(a.port)
    def run(cmd):
        for line in link.query(cmd): print(line)
        if a.errors:
            err = link.query("SYST:ERR?")[0]
            if not err.startswith("0,"): print("error:", err, file=sys.stderr)
    if a.commands:
        for c in a.commands: run(c)
        return
    print(f"connected via {link.via}; Ctrl+D quits", file=sys.stderr)
    while True:
        try: cmd = input("scpi> ").strip()
        except EOFError: print(); return
        if cmd: run(cmd)

if __name__ == "__main__":
    main()
