// NucleoLab multi-channel voltmeter TUI (hnoss).
//
// Talks SCPI to the board: through the bridge (127.0.0.1:5025, when ./run.sh
// is running) or straight to the ST-LINK serial port (921600 baud).
// Polls MEAS:VOLT? (@1:6) and shows every channel live; a channel the board
// reports as 9.91E+37 (NaN) has nothing plugged in. Plug/unplug events are
// logged. Anything typed in the console is sent as a SCPI command.
//
//   odin build . -out:nucleolab-tui && ./nucleolab-tui [/dev/ttyACMx]
package main

import "core:fmt"
import "core:os"
import "core:strconv"
import "core:strings"
import "core:sys/linux"
import "core:time"
import hnoss "libs/hnoss"
import "libs/hnoss/backend"

CHANNELS :: 6
NAMES := [CHANNELS]string{"A0", "A1", "A2", "A3", "A4", "A5"}
PINS := [CHANNELS]string{"PA0", "PA1", "PC3", "PC2", "PC1", "PC0"} // NUCLEO-L452RE-P

POLL_INTERVAL :: 40 * time.Millisecond
REPLY_TIMEOUT :: 5 * time.Second

ACCENT :: 0x00b5e2
WARM   :: 0xef3340
GOOD   :: 0x509e2f
GOLD   :: 0xf2a900
DIM    :: 0x7a8494

// --- Link: TCP to the bridge, or the serial port ---------------------------------------

Link_Kind :: enum { None, Bridge, Serial }
Txn_Kind :: enum { Poll, Config, User }

Link :: struct {
	kind:        Link_Kind,
	fd:          linux.Fd,
	serial_path: cstring,
	rx:          [dynamic]u8,
	// one transaction in flight; its replies end with "SYNC,<seq>"
	busy:        bool,
	txn:         Txn_Kind,
	seq:         int,
	sent:        time.Tick,
	user_cmd:    string,
	lines:       [dynamic]string,
}

// Kernel termios bits not in hnoss' backend (asm-generic/termbits.h)
CBAUD   :: 0o010017
B921600 :: 0o010007
CREAD   :: 0o000200
CLOCAL  :: 0o004000

link_connect :: proc(l: ^Link) -> bool {
	// 1. the bridge, so the web panel keeps working at the same time
	if fd, err := linux.socket(.INET, .STREAM, {.CLOEXEC}, .TCP); err == .NONE {
		addr := linux.Sock_Addr_In{sin_family = .INET, sin_port = 5025, sin_addr = {127, 0, 0, 1}}
		if linux.connect(fd, &addr) == .NONE {
			l.fd, l.kind = fd, .Bridge
			return link_ready(l)
		}
		linux.close(fd)
	}
	// 2. the serial port (fails with EBUSY while the bridge holds it)
	fd, err := linux.open(l.serial_path, {.RDWR, .NONBLOCK, .NOCTTY, .CLOEXEC})
	if err != .NONE { return false }
	t: backend.Termios
	if int(linux.ioctl(fd, backend.TCGETS, uintptr(&t))) < 0 {
		linux.close(fd)
		return false
	}
	t = backend.make_raw(t)
	t.cflag = (t.cflag &~ CBAUD) | B921600 | CREAD | CLOCAL
	t.cc[backend.VMIN] = 1 // with O_NONBLOCK: empty = EAGAIN, 0 bytes = hang-up
	linux.ioctl(fd, backend.TCSETS, uintptr(&t))
	l.fd, l.kind = fd, .Serial
	return link_ready(l)
}

link_ready :: proc(l: ^Link) -> bool {
	clear(&l.rx)
	l.busy = false
	// end any half line the board may still hold, and clear errors left in
	// its queue by an earlier session, so SYST:ERR? reports only our own
	link_write(l, "\n*CLS\n")
	return true
}

link_close :: proc(l: ^Link) {
	if l.kind != .None { linux.close(l.fd) }
	l.kind = .None
	l.busy = false
}

link_write :: proc(l: ^Link, s: string) -> bool {
	b := transmute([]u8)s
	for len(b) > 0 {
		n, err := linux.write(l.fd, b)
		if err == .EAGAIN { continue }
		if err != .NONE { return false }
		b = b[n:]
	}
	return true
}

link_send :: proc(l: ^Link, kind: Txn_Kind, payload: string) {
	l.seq = l.seq % 1_000_000_000 + 1
	l.txn, l.busy, l.sent = kind, true, time.tick_now()
	for s in l.lines { delete(s) }
	clear(&l.lines)
	if !link_write(l, fmt.tprintf("%s\n:SYST:SYNC? %d\n", payload, l.seq)) { link_close(l) }
}

// Reads what has arrived; returns true when the transaction in flight finished.
link_poll :: proc(l: ^Link) -> (done: bool) {
	if l.kind == .None { return }
	buf: [4096]u8
	for {
		fds := [1]linux.Poll_Fd{{fd = l.fd, events = {.IN}}}
		n, _ := linux.poll(fds[:], 0)
		if n <= 0 { break }
		k, err := linux.read(l.fd, buf[:])
		if err == .EAGAIN { break }
		if err != .NONE || k == 0 { link_close(l); return }
		append(&l.rx, ..buf[:k])
	}
	for {
		line, ok := next_reply(l)
		if !ok { break }
		if !l.busy { delete(line); continue } // stale output from before we connected
		if line == fmt.tprintf("SYNC,%d", l.seq) {
			delete(line)
			l.busy = false
			return true
		}
		append(&l.lines, line)
	}
	if l.busy && time.tick_since(l.sent) > REPLY_TIMEOUT { link_close(l) }
	return
}

// One reply from rx: a text line, or an IEEE 488.2 block (#<d><len><data>)
// summarised, since it may contain newlines
next_reply :: proc(l: ^Link) -> (string, bool) {
	rx := l.rx[:]
	if len(rx) >= 2 && rx[0] == '#' && rx[1] >= '1' && rx[1] <= '9' {
		d := int(rx[1] - '0')
		if len(rx) < 2 + d { return "", false }
		n, _ := strconv.parse_int(string(rx[2:2 + d]))
		total := 2 + d + n
		if len(rx) < total { return "", false }
		if len(rx) > total && rx[total] == '\n' { total += 1 }
		remove_range(&l.rx, 0, total)
		return fmt.aprintf("<%d bytes of binary data>", n), true
	}
	for b, i in rx {
		if b != '\n' { continue }
		line := strings.trim_right(string(rx[:i]), "\r")
		s := strings.clone(line)
		remove_range(&l.rx, 0, i + 1)
		return s, true
	}
	return "", false
}

// --- App state ---------------------------------------------------------------------------

HISTORY :: 4096

Chan :: struct {
	volts:     f64,
	conn:      bool,
	fail:      bool, // outside its limits (CALC:LIM:FAIL?)
	low, upp:  f64,  // its limits
	hist:      [dynamic]f64,
	min, max:  f64,
}

App :: struct {
	ch:        [CHANNELS]Chan,
	vdda:      f64,
	have:      bool,
	sel:       int,
	log:       hnoss.Log_State,
	cmd:       hnoss.Input_State,
	queue:     [dynamic]string, // user commands waiting to be sent
	history:   [dynamic]string,
	hist_pos:  int,
	last_poll: time.Tick,
	last_cfg:  time.Tick,
	lim_on:    bool,
	help:      bool, // F1 overlay
	// CSV recording (F2)
	rec:       ^os.File,
	rec_path:  string,
	rec_rows:  int,
	rec_start: time.Tick,
	polls:     int,
	rate:      int,
	rate_tick: time.Tick,
}

start_tick: time.Tick

// seconds since start: no time-zone guessing
stamp :: proc() -> string {
	t := time.duration_seconds(time.tick_since(start_tick))
	return fmt.tprintf("[#%06x]%.1fs[/]", DIM, t)
}

logf :: proc(app: ^App, format: string, args: ..any) {
	hnoss.log_append(&app.log, fmt.tprintf("%s %s", stamp(), fmt.tprintf(format, ..args)))
}

POLL_QUERY :: "MEAS:VOLT? (@1:6);:SYST:VDDA?;:CALC:LIM:FAIL?;:ROUT:CHAN:EVEN?"
CONFIG_QUERY :: "CALC:LIM?;:CALC:LIM:LOW? (@1:6);:CALC:LIM:UPP? (@1:6)"

// "v1,...,v6;vdda;fail1,...,fail6;ch,what,ms"
on_poll :: proc(app: ^App, lines: []string) {
	if len(lines) == 0 { return }
	parts := strings.split(lines[0], ";", context.temp_allocator)
	vals := strings.split(parts[0], ",", context.temp_allocator)
	if len(vals) != CHANNELS { return }
	if len(parts) > 1 { app.vdda, _ = strconv.parse_f64(parts[1]) }
	if len(parts) > 2 {
		for f, i in strings.split(parts[2], ",", context.temp_allocator) {
			if i < CHANNELS { app.ch[i].fail = f == "1" }
		}
	}
	if len(parts) > 3 { on_event(app, parts[3]) }
	for v, i in vals {
		x, ok := strconv.parse_f64(v)
		c := &app.ch[i]
		conn := ok && x < 1e30
		if conn && (!c.conn || !app.have) { c.min, c.max = x, x }
		c.conn = conn
		if conn {
			c.volts = x
			c.min = min(c.min, x)
			c.max = max(c.max, x)
		}
		append(&c.hist, x if conn else 0)
		if len(c.hist) > HISTORY { remove_range(&c.hist, 0, len(c.hist) - HISTORY) }
	}
	app.have = true
	app.polls += 1
	if app.rec != nil { record_row(app) }
}

// F2: start / stop writing every reading to nucleolab-<unix time>.csv
toggle_record :: proc(app: ^App) {
	if app.rec != nil {
		os.close(app.rec)
		app.rec = nil
		logf(app, "[#%06x]■ recording stopped[/] %s", ACCENT, dim(fmt.tprintf("%d rows in %s", app.rec_rows, app.rec_path)))
		return
	}
	delete(app.rec_path)
	app.rec_path = fmt.aprintf("nucleolab-%d.csv", time.time_to_unix(time.now()))
	f, err := os.create(app.rec_path)
	if err != nil {
		logf(app, "[#%06x]✗ cannot create %s: %v[/]", WARM, app.rec_path, err)
		return
	}
	app.rec, app.rec_rows, app.rec_start = f, 0, time.tick_now()
	header := strings.builder_make(context.temp_allocator)
	strings.write_string(&header, "time_s")
	for i in 0 ..< CHANNELS { fmt.sbprintf(&header, ",ch%d_%s_V", i + 1, NAMES[i]) }
	strings.write_string(&header, ",vdda_V\n")
	os.write_string(f, strings.to_string(header))
	logf(app, "[#%06x]● recording to %s[/]", WARM, app.rec_path)
}

// one row per poll; an empty cell where nothing is plugged in
record_row :: proc(app: ^App) {
	row := strings.builder_make(context.temp_allocator)
	fmt.sbprintf(&row, "%.3f", time.duration_seconds(time.tick_since(app.rec_start)))
	for c in app.ch {
		if c.conn { fmt.sbprintf(&row, ",%.4f", c.volts) } else { strings.write_byte(&row, ',') }
	}
	fmt.sbprintf(&row, ",%.4f\n", app.vdda)
	os.write_string(app.rec, strings.to_string(row))
	app.rec_rows += 1
}

// One entry of the board's event log: "<channel>,<what>,<ms since boot>"
on_event :: proc(app: ^App, ev: string) {
	f := strings.split(ev, ",", context.temp_allocator)
	if len(f) != 3 { return }
	ch, _ := strconv.parse_int(f[0])
	what, _ := strconv.parse_int(f[1])
	ms, _ := strconv.parse_int(f[2])
	if ch < 1 || ch > CHANNELS { return }
	i := ch - 1
	at := dim(fmt.tprintf("(board %.1fs)", f64(ms) / 1000))
	switch what {
	case 0: logf(app, "[#%06x]○ CH%d %s (%s) unplugged[/] %s", WARM, ch, NAMES[i], PINS[i], at)
	case 1: logf(app, "[#%06x]● CH%d %s (%s) plugged in[/] %s", GOOD, ch, NAMES[i], PINS[i], at)
	case 2: logf(app, "[#%06x]⚠ CH%d %s out of limits[/] %s", WARM, ch, NAMES[i], at)
	case 3: logf(app, "[#%06x]✓ CH%d %s back within limits[/] %s", GOOD, ch, NAMES[i], at)
	}
}

// "on;low1,...,low6;upp1,...,upp6"
on_config :: proc(app: ^App, lines: []string) {
	if len(lines) == 0 { return }
	parts := strings.split(lines[0], ";", context.temp_allocator)
	if len(parts) != 3 { return }
	app.lim_on = parts[0] == "1"
	lows := strings.split(parts[1], ",", context.temp_allocator)
	upps := strings.split(parts[2], ",", context.temp_allocator)
	if len(lows) != CHANNELS || len(upps) != CHANNELS { return }
	for i in 0 ..< CHANNELS {
		app.ch[i].low, _ = strconv.parse_f64(lows[i])
		app.ch[i].upp, _ = strconv.parse_f64(upps[i])
	}
}

on_user :: proc(app: ^App, cmd: string, lines: []string) {
	// the last line is the SYST:ERR? we appended
	n := len(lines)
	for line in lines[:max(n - 1, 0)] {
		logf(app, "[#%06x]←[/] %s", GOOD, line)
	}
	if n > 0 && !strings.has_prefix(lines[n - 1], "0,") {
		logf(app, "[#%06x]✗ %s[/]", WARM, lines[n - 1])
	} else if n <= 1 {
		logf(app, "[#%06x]✓ ok[/]", DIM)
	}
}

// --- Big digits ---------------------------------------------------------------------------

GLYPH_H :: 5

glyph :: proc(ch: rune) -> [GLYPH_H]string {
	switch ch {
	case '0': return {"███", "█ █", "█ █", "█ █", "███"}
	case '1': return {" ██", "  █", "  █", "  █", "  █"}
	case '2': return {"███", "  █", "███", "█  ", "███"}
	case '3': return {"███", "  █", "███", "  █", "███"}
	case '4': return {"█ █", "█ █", "███", "  █", "  █"}
	case '5': return {"███", "█  ", "███", "  █", "███"}
	case '6': return {"███", "█  ", "███", "█ █", "███"}
	case '7': return {"███", "  █", "  █", "  █", "  █"}
	case '8': return {"███", "█ █", "███", "█ █", "███"}
	case '9': return {"███", "█ █", "███", "  █", "███"}
	case '.': return {" ", " ", " ", " ", "█"}
	case 'V': return {"█ █", "█ █", "█ █", "█ █", " █ "}
	case '-': return {"   ", "   ", "███", "   ", "   "}
	}
	return {" ", " ", " ", " ", " "}
}

big_text :: proc(ui: ^hnoss.Ui, area: hnoss.Rect, s: string, color: u32) {
	rows: [GLYPH_H]strings.Builder
	for &r in rows { strings.builder_init(&r, context.temp_allocator) }
	for ch in s {
		g := glyph(ch)
		for i in 0 ..< GLYPH_H {
			strings.write_string(&rows[i], g[i])
			strings.write_byte(&rows[i], ' ')
		}
	}
	width := strings.rune_count(strings.to_string(rows[0]))
	box := hnoss.center(area, width, GLYPH_H)
	for i in 0 ..< GLYPH_H {
		hnoss.label(ui, {box.x, box.y + i, box.width, 1}, fmt.tprintf("[#%06x]%s[/]", color, strings.to_string(rows[i])), wrap = .Clip)
	}
}

// --- Drawing ------------------------------------------------------------------------------

dim :: proc(s: string) -> string { return fmt.tprintf("[#%06x]%s[/]", DIM, s) }

draw :: proc(ui: ^hnoss.Ui, area_: hnoss.Rect, app: ^App, l: ^Link) {
	area := area_

	head := hnoss.cut_top(&area, 1)
	status: string
	switch l.kind {
	case .Bridge: status = fmt.tprintf("[#%06x]● bridge[/] %s", GOOD, dim(fmt.tprintf("127.0.0.1:5025 · %d reads/s", app.rate)))
	case .Serial: status = fmt.tprintf("[#%06x]● serial[/] %s", GOOD, dim(fmt.tprintf("%s · %d reads/s", l.serial_path, app.rate)))
	case .None:   status = fmt.tprintf("[#%06x]● offline[/] %s", WARM, dim("waiting for board / bridge…"))
	}
	if app.rec != nil { status = fmt.tprintf("[b #%06x]● REC %d[/] %s", WARM, app.rec_rows, status) }
	if app.vdda > 0 { status = fmt.tprintf("%s %s", dim(fmt.tprintf("VDDA %.3f V ·", app.vdda)), status) }
	hnoss.label(ui, hnoss.cut_right(&head, 76), status, align = .Right, wrap = .Clip)
	hnoss.label(ui, head, fmt.tprintf("[b]NucleoLab[/] %s", dim("multi-channel voltmeter · SCPI")), wrap = .Clip)

	foot := hnoss.cut_bottom(&area, 1)
	k := fmt.tprintf("#%06x", ACCENT)
	hnoss.label(ui, foot, fmt.tprintf("[%s]F1[/] help  [%s]Tab[/] channel  [%s]Enter[/] send  [%s]F2[/] %s  [%s]Esc[/] quit", k, k, k, k, "stop recording" if app.rec != nil else "record CSV", k), wrap = .Clip)

	rows := hnoss.split(area, .Vertical, [?]hnoss.Size{CHANNELS + 3, hnoss.fill()})
	draw_channels(ui, rows[0], app)
	bottom := hnoss.split(rows[1], .Horizontal, [?]hnoss.Size{hnoss.fill(3), hnoss.fill(2)})
	draw_selected(ui, bottom[0], app)
	draw_console(ui, bottom[1], app)
}

draw_channels :: proc(ui: ^hnoss.Ui, r: hnoss.Rect, app: ^App) {
	inner := hnoss.box(ui, r, fmt.tprintf("[b]Channels[/] %s", dim("auto-detect")))
	hdr := hnoss.cut_top(&inner, 1)
	hnoss.label(ui, hdr, dim("  CHANNEL PIN    STATE            VOLTAGE"), wrap = .Clip)
	vmax := app.vdda if app.vdda > 0 else 3.3
	for i in 0 ..< CHANNELS {
		row := hnoss.cut_top(&inner, 1)
		c := &app.ch[i]
		mark := fmt.tprintf("[#%06x]▶[/]", ACCENT) if i == app.sel else " "
		name := fmt.tprintf("[b]CH%d[/] %s", i + 1, NAMES[i]) if c.conn else dim(fmt.tprintf("CH%d %s", i + 1, NAMES[i]))
		state: string
		switch {
		case !app.have:          state = dim("…")
		case c.conn && c.fail:   state = fmt.tprintf("[#%06x]⚠ out of limits[/]", WARM)
		case c.conn:             state = fmt.tprintf("[#%06x]● connected[/]", GOOD)
		case:                    state = dim("○ empty")
		}
		value := dim("— V")
		if c.conn { value = fmt.tprintf("[b #%06x]%.3f V[/]", WARM, c.volts) if c.fail else fmt.tprintf("[b]%.3f V[/]", c.volts) }
		hnoss.label(ui, hnoss.cut_left(&row, 2), mark)
		hnoss.label(ui, hnoss.cut_left(&row, 8), name, wrap = .Clip)
		hnoss.label(ui, hnoss.cut_left(&row, 7), dim(PINS[i]), wrap = .Clip)
		hnoss.label(ui, hnoss.cut_left(&row, 15), state, wrap = .Clip)
		hnoss.label(ui, hnoss.cut_left(&row, 9), value, align = .Right, wrap = .Clip)
		hnoss.cut_left(&row, 2)
		hnoss.cut_left(&row, 1)
		if !c.conn { continue }
		spark := hnoss.cut_right(&row, min(24, row.width / 3))
		hnoss.cut_right(&row, 1)
		hnoss.gauge(ui, row, c.volts / vmax, "")
		data := c.hist[max(len(c.hist) - spark.width, 0):]
		hnoss.sparkline(ui, spark, data, vmax)
	}
}

draw_selected :: proc(ui: ^hnoss.Ui, r: hnoss.Rect, app: ^App) {
	c := &app.ch[app.sel]
	inner := hnoss.box(ui, r, fmt.tprintf("[b]CH%d · %s[/] %s", app.sel + 1, NAMES[app.sel], dim(PINS[app.sel])))
	top := hnoss.cut_top(&inner, GLYPH_H + 2)
	if c.conn {
		big_text(ui, top, fmt.tprintf("%.3fV", c.volts), WARM if c.fail else ACCENT)
		mm := hnoss.cut_top(&inner, 1)
		stats := fmt.tprintf("min %.3f V · max %.3f V · span %.3f V", c.min, c.max, c.max - c.min)
		if app.lim_on {
			lim := fmt.tprintf("limits %.3f – %.3f V", c.low, c.upp)
			stats = fmt.tprintf("%s · %s", stats, fmt.tprintf("[#%06x]%s[/]", WARM, lim) if c.fail else lim)
		}
		hnoss.label(ui, mm, dim(stats), align = .Center, wrap = .Clip)
	} else {
		msg := "empty · plug a signal in, it is detected automatically"
		hnoss.label(ui, hnoss.center(top, len(msg), 1), dim(msg), align = .Center, wrap = .Clip)
		hnoss.cut_top(&inner, 1)
	}
	vmax := app.vdda if app.vdda > 0 else 3.3
	if inner.height >= 2 {
		grad := [?]hnoss.Rgb{hnoss.hex(GOOD), hnoss.hex(ACCENT), hnoss.hex(WARM)}
		hnoss.graph(ui, inner, c.hist[:], {max_value = vmax, gradient = grad[:]})
	}
}

HELP_KEYS :: [?][2]string{
	{"Enter", "send the command in the console"},
	{"↑ ↓", "command history"},
	{"Tab / Shift+Tab", "select channel"},
	{"F1", "this help"},
	{"F2", "start / stop CSV recording"},
	{"Esc", "close help · quit"},
}
HELP_CMDS :: [?][2]string{
	{"MEAS:VOLT? (@1,4:6)", "volts; 9.91E+37 = nothing plugged in"},
	{"ROUT:CHAN:CAT?", "connected channels"},
	{"CALC:LIM:UPP 2.5,(@1)", "upper limit for CH1 (LOW likewise)"},
	{"CALC:LIM ON", "check limits, alarms turn red"},
	{"CALC:AVER? (@1:6)", "mean since plug-in (MIN? MAX? COUN?)"},
	{"VOLT:AVER:COUN 16", "samples per MEAS:VOLT?"},
	{"ROUT:CHAN:DET OFF", "no plug detection, all connected"},
	{"SYST:HELP:HEAD?", "every command the board knows"},
}

draw_help :: proc(ui: ^hnoss.Ui, area: hnoss.Rect, app: ^App) {
	h := len(HELP_KEYS) + len(HELP_CMDS) + 3
	inner := hnoss.modal(ui, hnoss.center(area, 72, h + 2), &app.help, "[b]Help[/]")
	row := 0
	line :: proc(ui: ^hnoss.Ui, inner: hnoss.Rect, row: ^int, left, right: string, width: int) {
		r := hnoss.Rect{inner.x + 1, inner.y + row^, inner.width - 2, 1}
		hnoss.label(ui, hnoss.cut_left(&r, width), left, wrap = .Clip)
		hnoss.label(ui, r, dim(right), wrap = .Clip)
		row^ += 1
	}
	hnoss.label(ui, {inner.x + 1, inner.y, inner.width - 2, 1}, fmt.tprintf("[b #%06x]Keys[/]", ACCENT))
	row += 1
	for k in HELP_KEYS { line(ui, inner, &row, fmt.tprintf("[b]%s[/]", k[0]), k[1], 18) }
	row += 1
	hnoss.label(ui, {inner.x + 1, inner.y + row, inner.width - 2, 1}, fmt.tprintf("[b #%06x]SCPI[/]", ACCENT))
	row += 1
	for c in HELP_CMDS { line(ui, inner, &row, c[0], c[1], 26) }
}

draw_console :: proc(ui: ^hnoss.Ui, r: hnoss.Rect, app: ^App) {
	inner := hnoss.box(ui, r, "[b]SCPI console[/]", {focused = true})
	line := hnoss.cut_bottom(&inner, 1)
	hnoss.label(ui, hnoss.cut_left(&line, 2), fmt.tprintf("[#%06x]›[/]", ACCENT))
	hnoss.focus(ui, &app.cmd)
	hnoss.input(ui, line, &app.cmd, {placeholder = "MEAS:VOLT? (@1:6) · ROUT:CHAN:CAT? · *IDN?"})
	hnoss.log_view(ui, inner, &app.log, {markup = true, wrap = .Word})
}

// --- Main ---------------------------------------------------------------------------------

main :: proc() {
	context.assertion_failure_proc = backend.assertion_failure_proc

	path := os.args[1] if len(os.args) > 1 else "/dev/ttyACM0"
	link := Link{serial_path = strings.clone_to_cstring(path)}
	app := App{}
	app.log.max_lines = 500
	app.rate_tick = time.tick_now()
	start_tick = time.tick_now()
	logf(&app, "%s", dim("Try: MEAS:VOLT? (@1,4:6) · ROUT:CHAN:CAT? · ROUT:CHAN:CONN? · SYST:HELP:HEAD?"))

	term, err := hnoss.init()
	if err != .None {
		fmt.eprintfln("hnoss: init failed: %v", err)
		os.exit(1)
	}
	defer hnoss.deinit(&term)
	hnoss.set_title(&term, "NucleoLab voltmeter")

	events: []hnoss.Event
	last_try: time.Tick
	was: Link_Kind
	loop: for {
		// connection
		if link.kind == .None && time.tick_since(last_try) > 500 * time.Millisecond {
			last_try = time.tick_now()
			link_connect(&link)
		}
		if link.kind != was {
			switch link.kind {
			case .Bridge: logf(&app, "[#%06x]connected to bridge (127.0.0.1:5025)[/]", GOOD)
			case .Serial: logf(&app, "[#%06x]connected to serial port (%s)[/]", GOOD, path)
			case .None:   logf(&app, "[#%06x]connection lost[/]", WARM); app.have = false
			}
			was = link.kind
		}

		// traffic: user commands first, otherwise poll the channels
		if link_poll(&link) {
			switch link.txn {
			case .Poll:   on_poll(&app, link.lines[:])
			case .Config: on_config(&app, link.lines[:])
			case .User: on_user(&app, link.user_cmd, link.lines[:])
			}
		}
		if link.kind != .None && !link.busy {
			if len(app.queue) > 0 {
				cmd := pop_front(&app.queue)
				delete(link.user_cmd)
				link.user_cmd = cmd
				link_send(&link, .User, fmt.tprintf("%s\nSYST:ERR?", cmd))
			} else if time.tick_since(app.last_cfg) > time.Second {
				app.last_cfg = time.tick_now()
				link_send(&link, .Config, CONFIG_QUERY)
			} else if time.tick_since(app.last_poll) > POLL_INTERVAL {
				app.last_poll = time.tick_now()
				link_send(&link, .Poll, POLL_QUERY)
			}
		}
		if time.tick_since(app.rate_tick) >= time.Second {
			app.rate, app.polls, app.rate_tick = app.polls, 0, time.tick_now()
		}

		f := hnoss.begin_frame(&term)
		ui := hnoss.ui(f, events, hnoss.ODLAR_YURDU_THEME)
		draw(ui, f.area, &app, &link)
		if app.help { draw_help(ui, f.area, &app) }

		if app.cmd.submitted {
			cmd := strings.trim_space(hnoss.input_text(&app.cmd))
			if len(cmd) > 0 {
				logf(&app, "[#%06x]→[/] [b]%s[/]", ACCENT, cmd)
				append(&app.queue, strings.clone(cmd))
				if len(app.history) == 0 || app.history[len(app.history) - 1] != cmd {
					append(&app.history, strings.clone(cmd))
				}
				if link.kind == .None { logf(&app, "[#%06x]offline, will send once connected[/]", WARM) }
			}
			app.hist_pos = len(app.history)
			hnoss.input_set(&app.cmd, "")
		}

		for ev in hnoss.unhandled(ui) {
			switch {
			case hnoss.is_key(ev, .Esc) || hnoss.is_char(ev, 'c', {.Ctrl}):
				break loop
			case hnoss.is_key(ev, .F1):
				app.help = !app.help
			case hnoss.is_key(ev, .F2):
				toggle_record(&app)
			case hnoss.is_key(ev, .Tab):
				app.sel = (app.sel + 1) % CHANNELS
			case hnoss.is_key(ev, .Back_Tab):
				app.sel = (app.sel + CHANNELS - 1) % CHANNELS
			case hnoss.is_key(ev, .Up) && app.hist_pos > 0:
				app.hist_pos -= 1
				hnoss.input_set(&app.cmd, app.history[app.hist_pos])
			case hnoss.is_key(ev, .Down) && app.hist_pos < len(app.history):
				app.hist_pos += 1
				hnoss.input_set(&app.cmd, app.history[app.hist_pos] if app.hist_pos < len(app.history) else "")
			}
		}
		hnoss.end_frame(&term)
		free_all(context.temp_allocator)
		events = hnoss.poll_events(&term, 16)
		if term.error != .None { break }
	}
	if app.rec != nil { os.close(app.rec) }
	link_close(&link)
}
