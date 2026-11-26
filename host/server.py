"""NucleoLab bridge: web panel on http://127.0.0.1:8080 and raw SCPI socket on 127.0.0.1:5025."""
import argparse, asyncio, glob, itertools, json, os, struct, time
import serial
from aiohttp import web, WSMsgType

HERE = os.path.dirname(os.path.abspath(__file__))

class Instrument:
    """Serialises access to the board. A SYNC echo marks the end of every reply."""
    def __init__(self, port):
        self.port, self.ser, self.lock, self.seq = port, None, asyncio.Lock(), itertools.count(int(time.time()) % 10**6 * 1000)
        self.remote_until = 0.0

    def _open(self):
        path = self.port or next(iter(glob.glob("/dev/serial/by-id/*STLink*")), None)
        if not path: raise OSError("ST-LINK seri portu bulunamadı")
        self.ser = serial.Serial(path, 921600, timeout=.05, exclusive=True)
        self.ser.write(b"\n"); time.sleep(.6); self.ser.reset_input_buffer()

    def _txn(self, line, timeout):
        try:
            if self.ser is None: self._open()
            tag = b"SYNC,%d\n" % (next(self.seq) % 2_000_000_000)
            self.ser.write(line + b"\n:SYST:SYNC? " + tag[5:])
            buf, end = bytearray(), time.monotonic() + timeout
            while not buf.endswith(tag):
                buf += self.ser.read(max(1, self.ser.in_waiting))
                if time.monotonic() > end: raise TimeoutError("cihaz yanıt vermedi")
            return bytes(buf[:-len(tag)])
        except (OSError, serial.SerialException):
            if self.ser: self.ser.close()
            self.ser = None
            raise

    async def txn(self, line, timeout=25.0):
        async with self.lock:
            job = asyncio.ensure_future(asyncio.to_thread(self._txn, line, timeout))
            try: return await asyncio.shield(job)
            except asyncio.CancelledError:      # never release the lock mid-reply
                await asyncio.wait([job]); raise

def take_block(b):
    d = int(b[1:2]); n = int(b[2:2 + d])
    return b[2 + d:2 + d + n], b[2 + d + n:]

def split_message(buf):
    """One program message from buf (same block/quote rules as the firmware), or None."""
    i, q = 0, 0
    while i < len(buf):
        c = buf[i]
        if c == 10: return bytes(buf[:i]), buf[i + 1:]
        if q: q = 0 if c == q else q
        elif c in b"\"'": q = c
        elif c == 35 and i and buf[i - 1] in b" \t\r,":
            if i + 1 >= len(buf): return None
            if 49 <= buf[i + 1] <= 57:
                d = buf[i + 1] - 48
                digits = bytes(buf[i + 2:i + 2 + d])
                if digits.isdigit() or not digits:
                    if len(digits) < d: return None
                    if int(digits) <= 8448:
                        i += 2 + d + int(digits)
                        if i > len(buf): return None
                        continue
        i += 1
    return None

async def tcp_client(ins, rd, wr):
    buf = bytearray()
    try:
        while data := await rd.read(65536):
            buf += data
            while (m := split_message(buf)):
                line, buf = m
                ins.remote_until = time.monotonic() + 2
                wr.write(await ins.txn(line))
                ins.remote_until = time.monotonic() + 2
                await wr.drain()
            if len(buf) > 65536: break
    except (ConnectionError, OSError): pass
    finally: wr.close()

async def frame(ins, mode):
    cmd = b"ACQ:STAT?;:INIT;*OPC?;:WAV:PRE?;DATA?" + (b";:CALC:DIST?;SPEC?" if mode == "spec" else b"")
    r = await ins.txn(cmd)
    i = r.find(b"#")
    if i < 0:
        e = (await ins.txn(b"SYST:ERR?")).decode("ascii", "replace").strip()
        return {"type": "error", "text": e}
    head = r[:i].decode().split(";")[2].split(",")
    raw, rest = take_block(r[i:])
    out = {"type": "frame", "dt": float(head[1]), "lsb": float(head[2]), "pre": int(head[3]), "trig": head[4] == "1",
           "y": struct.unpack(f"<{len(raw)//2}H", raw)}
    if mode == "spec" and b"#" in rest:
        j = rest.index(b"#")
        out["dist"] = [float(x) for x in rest[:j].decode().strip(";").split(",")]
        s, _ = take_block(rest[j:])
        out["spec"] = struct.unpack(f"<{len(s)//2}h", s)
    return out

async def ws_handler(req):
    ins, host = req.app["ins"], req.headers.get("Host", "")
    # Reject other web sites and DNS rebinding: this socket drives hardware.
    if host.rsplit(":", 1)[0] not in ("127.0.0.1", "localhost") or req.headers.get("Origin") != "http://" + host:
        raise web.HTTPForbidden()
    ws = web.WebSocketResponse(heartbeat=20)
    await ws.prepare(req)
    st = {"run": False, "mode": "scope"}

    async def stream():
        was = None
        while not ws.closed:
            try:
                if not st["run"]: await asyncio.sleep(.05); continue
                remote = time.monotonic() < ins.remote_until
                if remote != was: await ws.send_json({"type": "remote", "on": remote}); was = remote
                if remote: await asyncio.sleep(.2); continue
                await ws.send_json(await frame(ins, st["mode"]))
                await asyncio.sleep(0)
            except (OSError, ValueError, IndexError) as e:
                if ws.closed: return
                await ws.send_json({"type": "error", "text": str(e)}); await asyncio.sleep(1)
    task = asyncio.create_task(stream())
    try:
        async for msg in ws:
            if msg.type != WSMsgType.TEXT: continue
            d = json.loads(msg.data)
            if "run" in d: st["run"] = bool(d["run"])
            if "mode" in d: st["mode"] = d["mode"]
            if "line" in d or "arb" in d:
                rep = {"type": "reply", "id": d.get("id")}
                try:
                    if "arb" in d:      # waveform drawn in the panel
                        codes = [int(c) for c in d["arb"]]
                        if not (2 <= len(codes) <= 4096 and all(0 <= c <= 4095 for c in codes)):
                            raise ValueError("geçersiz dalga verisi")
                        raw = struct.pack(f"<{len(codes)}H", *codes); size = str(len(raw))
                        await ins.txn(b"SOUR:DATA:ARB #%d%s" % (len(size), size.encode()) + raw)
                        d["line"] = "SOUR:FUNC ARB"
                    line = d["line"].replace("\n", " ").encode("ascii", "replace")
                    r = await ins.txn(line)
                    rep["text"] = (f"<{len(r)} bayt ikili veri> " if b"#" in r and any(c > 126 for c in r) else "") + r[:4000].decode("ascii", "replace").strip()
                    errs = []
                    if "SYST:ERR" not in d["line"].upper():
                        for _ in range(16):
                            e = (await ins.txn(b"SYST:ERR?")).decode().strip()
                            if e.startswith("0,"): break
                            errs.append(e)
                    rep["errors"] = errs
                except (OSError, ValueError) as e: rep.update(text="", errors=[str(e)])
                await ws.send_json(rep)
    finally:
        task.cancel()
    return ws

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial"); ap.add_argument("--http", type=int, default=8080); ap.add_argument("--scpi", type=int, default=5025)
    a = ap.parse_args()
    ins = Instrument(a.serial)
    print("Cihaz:", (await ins.txn(b"*RST;*CLS;*IDN?")).decode().strip())
    loop = float((await ins.txn(b"DIAG:LOOP?")).decode().split(",")[0])
    ch = 1 if loop < .03 else 3
    print("Jumper PA4-PA0:", "takılı, kanal 1" if ch == 1 else "yok, dahili kanal 3 kullanılıyor")
    await ins.txn(b"ACQ:CHAN %d;POIN 2048;SRAT 5E5;:SOUR:FREQ 1000;:OUTP ON" % ch)
    app = web.Application(); app["ins"] = ins
    app.router.add_get("/", lambda r: web.FileResponse(os.path.join(HERE, "panel.html"), headers={"Cache-Control": "no-cache"}))
    app.router.add_get("/ws", ws_handler)
    run = web.AppRunner(app); await run.setup()
    await web.TCPSite(run, "127.0.0.1", a.http).start()
    srv = await asyncio.start_server(lambda r, w: tcp_client(ins, r, w), "127.0.0.1", a.scpi)
    print(f"Panel:  http://127.0.0.1:{a.http}\nPyVISA: TCPIP::127.0.0.1::{a.scpi}::SOCKET", flush=True)
    async with srv: await srv.serve_forever()

if __name__ == "__main__":
    try: asyncio.run(main())
    except KeyboardInterrupt: pass
    except OSError as e: raise SystemExit(f"Hata: {e}")
