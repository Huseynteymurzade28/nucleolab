// Live potentiometer monitor for the NUCLEO-L452RE pot-adc firmware.
// Reads "P <raw> <pot_mv> <vdd_mv>" lines from the ST-LINK serial port
// (/dev/ttyACM0, 115200) and draws them with hnoss.
//
//   odin run . -- [/dev/ttyACMx]
//
// Keys: q quit · space pause · c reset min/max · + / - graph zoom
package main

import "core:fmt"
import "core:os"
import "core:strconv"
import "core:strings"
import "core:sys/linux"
import hnoss "../../../tui/libs/hnoss"
import "../../../tui/libs/hnoss/backend"

// --- Serial port -----------------------------------------------------------------

// Kernel termios bits not in hnoss' backend (asm-generic/termbits.h)
CBAUD   :: 0o010017
B115200 :: 0o010002
CREAD   :: 0o000200
CLOCAL  :: 0o004000

Serial :: struct {
	path: cstring,
	fd:   linux.Fd,
	line: [dynamic]u8, // bytes of the current, unfinished line
}

serial_open :: proc(s: ^Serial) -> bool {
	fd, err := linux.open(s.path, {.NONBLOCK, .NOCTTY, .CLOEXEC})
	if err != .NONE { return false }
	t: backend.Termios
	if int(linux.ioctl(fd, backend.TCGETS, uintptr(&t))) < 0 {
		linux.close(fd)
		return false
	}
	t = backend.make_raw(t)
	t.cflag = (t.cflag &~ CBAUD) | B115200 | CREAD | CLOCAL
	// VMIN = 1: with O_NONBLOCK an empty port then gives EAGAIN, so a read
	// of 0 bytes really means hang-up (board unplugged)
	t.cc[backend.VMIN] = 1
	linux.ioctl(fd, backend.TCSETS, uintptr(&t))
	s.fd = fd
	clear(&s.line)
	return true
}

serial_close :: proc(s: ^Serial) {
	if s.fd >= 0 { linux.close(s.fd) }
	s.fd = -1
}

Sample :: struct { raw, mv, vdd: int }

// Reads everything that has arrived; calls `on_sample` for each full line.
// Returns false when the port is gone (board unplugged).
serial_poll :: proc(s: ^Serial, app: ^App) -> bool {
	buf: [4096]u8
	for {
		n, err := linux.read(s.fd, buf[:])
		if err == .EAGAIN { return true }
		if err != .NONE || n == 0 { return false }
		for b in buf[:n] {
			if b == '\n' {
				if smp, ok := parse_line(string(s.line[:])); ok { on_sample(app, smp) }
				clear(&s.line)
			} else if len(s.line) < 64 {
				append(&s.line, b)
			}
		}
	}
}

parse_line :: proc(line: string) -> (s: Sample, ok: bool) {
	f := strings.fields(line, context.temp_allocator)
	if len(f) != 4 || f[0] != "P" { return }
	s.raw = strconv.parse_int(f[1]) or_return
	s.mv = strconv.parse_int(f[2]) or_return
	s.vdd = strconv.parse_int(f[3]) or_return
	return s, s.raw >= 0 && s.raw <= 4095
}

// --- App state ---------------------------------------------------------------------

HISTORY :: 4096

App :: struct {
	last:      Sample,
	have:      bool,
	paused:    bool,
	min_mv:    int,
	max_mv:    int,
	// samples of the current frame, averaged into one history point
	acc_mv:    int,
	acc_n:     int,
	history:   [dynamic]f64, // volts, one point per frame
	zoom:      int,          // frames per graph point
	// rate
	count:     int,
	rate:      int,
	rate_tick: int,
}

on_sample :: proc(app: ^App, s: Sample) {
	app.count += 1
	if app.paused { return }
	app.last = s
	if !app.have || s.mv < app.min_mv { app.min_mv = s.mv }
	if !app.have || s.mv > app.max_mv { app.max_mv = s.mv }
	app.have = true
	app.acc_mv += s.mv
	app.acc_n += 1
}

end_of_frame :: proc(app: ^App) {
	if app.paused || app.acc_n == 0 { return }
	append(&app.history, f64(app.acc_mv) / f64(app.acc_n) / 1000)
	app.acc_mv, app.acc_n = 0, 0
	if len(app.history) > HISTORY {
		remove_range(&app.history, 0, len(app.history) - HISTORY)
	}
}

// Every zoom-th point (averaged), so + / - shows a longer or shorter time span
zoomed :: proc(app: ^App) -> []f64 {
	if app.zoom <= 1 { return app.history[:] }
	out := make([dynamic]f64, context.temp_allocator)
	for i := len(app.history) % app.zoom; i + app.zoom <= len(app.history); i += app.zoom {
		sum := 0.0
		for v in app.history[i:i + app.zoom] { sum += v }
		append(&out, sum / f64(app.zoom))
	}
	return out[:]
}

// --- Big digits ------------------------------------------------------------------------

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
		line := hnoss.Rect{box.x, box.y + i, box.width, 1}
		hnoss.label(ui, line, fmt.tprintf("[#%06x]%s[/]", color, strings.to_string(rows[i])), wrap = .Clip)
	}
}

// --- Drawing ---------------------------------------------------------------------------

ACCENT :: 0x00b5e2 // Azerbaijani flag blue
WARM   :: 0xef3340 // red
GOOD   :: 0x509e2f // green
DIM    :: 0x7a8494

draw :: proc(ui: ^hnoss.Ui, area_: hnoss.Rect, app: ^App, connected: bool, port: string) {
	area := area_

	// header
	head := hnoss.cut_top(&area, 1)
	status := fmt.tprintf("[#%06x]● connected[/] [#%06x]%s · %d samples/s[/]", GOOD, DIM, port, app.rate) if connected \
		else fmt.tprintf("[#%06x]● offline[/] [#%06x]waiting for %s…[/]", WARM, DIM, port)
	hnoss.label(ui, hnoss.cut_right(&head, 48), status, align = .Right, wrap = .Clip)
	hnoss.label(ui, head, "[b]Potentiometer[/] [#7a8494]NUCLEO-L452RE-P · A0 (PA0)[/]", wrap = .Clip)

	// footer
	foot := hnoss.cut_bottom(&area, 1)
	k := fmt.tprintf("#%06x", ACCENT)
	hnoss.label(ui, foot, fmt.tprintf(
		"[%s]q[/] quit  [%s]space[/] %s  [%s]c[/] reset min/max  [%s]+ -[/] time scale ×%d",
		k, k, "resume" if app.paused else "pause", k, k, app.zoom), wrap = .Clip)

	rows := hnoss.split(area, .Vertical, [?]hnoss.Size{9, 3, hnoss.fill()})
	top := hnoss.split(rows[0], .Horizontal, [?]hnoss.Size{hnoss.fill(3), hnoss.fill(2)})

	// big voltage
	title := "[b]Voltage[/] [#ef3340]PAUSED[/]" if app.paused else "[b]Voltage[/]"
	big := hnoss.box(ui, top[0], title)
	if app.have {
		big_text(ui, big, fmt.tprintf("%.3fV", f64(app.last.mv) / 1000), ACCENT)
	} else {
		hnoss.label(ui, hnoss.center(big, 20, 1), "[#7a8494]waiting for data…[/]", align = .Center)
	}

	// details
	info := hnoss.box(ui, top[1], "[b]Details[/]")
	if app.have {
		line :: proc(ui: ^hnoss.Ui, r: ^hnoss.Rect, name, value: string) {
			l := hnoss.cut_top(r, 1)
			hnoss.label(ui, hnoss.cut_left(&l, 10), fmt.tprintf("[#7a8494]%s[/]", name), wrap = .Clip)
			hnoss.label(ui, l, value, align = .Right, wrap = .Clip)
		}
		s := app.last
		line(ui, &info, "ADC raw", fmt.tprintf("%d / 4095", s.raw))
		line(ui, &info, "Position", fmt.tprintf("%.1f %%", f64(s.raw) * 100 / 4095))
		line(ui, &info, "VDD", fmt.tprintf("%.3f V", f64(s.vdd) / 1000))
		line(ui, &info, "Min", fmt.tprintf("%.3f V", f64(app.min_mv) / 1000))
		line(ui, &info, "Max", fmt.tprintf("%.3f V", f64(app.max_mv) / 1000))
		line(ui, &info, "Span", fmt.tprintf("%.3f V", f64(app.max_mv - app.min_mv) / 1000))
	}

	// position gauge
	g := hnoss.box(ui, rows[1], "[b]Position[/]")
	ratio := f64(app.last.raw) / 4095 if app.have else 0
	hnoss.gauge(ui, g, ratio, fmt.tprintf("%.1f %%", ratio * 100))

	// history graph
	vdd := f64(app.last.vdd) / 1000 if app.have && app.last.vdd > 0 else 3.3
	gr := hnoss.box(ui, rows[2], fmt.tprintf("[b]History[/] [#7a8494]0 – %.2f V[/]", vdd))
	if gr.width > 7 {
		axis := hnoss.cut_left(&gr, 6)
		hnoss.label(ui, {axis.x, axis.y, axis.width, 1}, fmt.tprintf("[#7a8494]%.1fV[/]", vdd), wrap = .Clip)
		hnoss.label(ui, {axis.x, axis.y + axis.height / 2, axis.width, 1}, fmt.tprintf("[#7a8494]%.1fV[/]", vdd / 2), wrap = .Clip)
		hnoss.label(ui, {axis.x, axis.y + axis.height - 1, axis.width, 1}, "[#7a8494]0V[/]", wrap = .Clip)
	}
	grad := [?]hnoss.Rgb{hnoss.hex(GOOD), hnoss.hex(ACCENT), hnoss.hex(WARM)}
	hnoss.graph(ui, gr, zoomed(app), {max_value = vdd, gradient = grad[:]})
}

// --- Main ---------------------------------------------------------------------------------

main :: proc() {
	context.assertion_failure_proc = backend.assertion_failure_proc

	port := os.args[1] if len(os.args) > 1 else "/dev/ttyACM0"
	ser := Serial{path = strings.clone_to_cstring(port), fd = -1}
	app := App{zoom = 1}

	term, err := hnoss.init()
	if err != .None {
		fmt.eprintfln("hnoss: init failed: %v", err)
		os.exit(1)
	}
	defer hnoss.deinit(&term)
	hnoss.set_title(&term, "Potentiometer")

	events: []hnoss.Event
	frame := 0
	loop: for {
		// serial: (re)connect, then drain everything that arrived
		if ser.fd < 0 && frame % 15 == 0 { serial_open(&ser) }
		if ser.fd >= 0 && !serial_poll(&ser, &app) { serial_close(&ser) }
		end_of_frame(&app)

		// samples per second, updated once a second (30 frames)
		app.rate_tick += 1
		if app.rate_tick >= 30 {
			app.rate, app.count, app.rate_tick = app.count, 0, 0
		}

		f := hnoss.begin_frame(&term)
		ui := hnoss.ui(f, events, hnoss.ODLAR_YURDU_THEME)
		draw(ui, f.area, &app, ser.fd >= 0, port)

		for ev in hnoss.unhandled(ui) {
			switch {
			case hnoss.is_char(ev, 'q') || hnoss.is_key(ev, .Esc) || hnoss.is_char(ev, 'c', {.Ctrl}):
				break loop
			case hnoss.is_char(ev, ' '):
				app.paused = !app.paused
			case hnoss.is_char(ev, 'c'):
				app.have = false
			case hnoss.is_char(ev, '+') || hnoss.is_char(ev, '='):
				app.zoom = min(app.zoom * 2, 64)
			case hnoss.is_char(ev, '-'):
				app.zoom = max(app.zoom / 2, 1)
			}
		}
		hnoss.end_frame(&term)
		free_all(context.temp_allocator)
		events = hnoss.poll_events(&term, 33)
		if term.error != .None { break }
		frame += 1
	}
	serial_close(&ser)
}
