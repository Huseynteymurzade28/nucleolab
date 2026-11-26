# Development

## Toolchain

| For | Needs |
|---|---|
| firmware | `arm-none-eabi-gcc` + newlib, `st-flash` (or OpenOCD) |
| TUIs | [Odin](https://odin-lang.org) `dev-2026-10` |
| Zig prototype | [Zig](https://ziglang.org) 0.17 |
| bridge, tools | Python 3, `pip install -r requirements.txt` |

Clone with `--recursive`: CMSIS (ST) and hnoss are submodules.

## Layout

| Path | What |
|---|---|
| `fw/scpi` | Portable SCPI / IEEE 488.2 core: no heap, no hardware |
| `fw/board` | Clock (80 MHz PLL, LSE-trimmed MSI), UART, ADC / DAC / DMA, plug probe |
| `fw/app/lab.c` | Generator, scope, measurements, Bode |
| `fw/app/spectrum.c` | FFT, THD / SNR / SINAD |
| `fw/app/dvm.c` | Voltmeter: detection, background sampling, statistics, limits, events |
| `host` | Bridge (serial ↔ TCP 5025 + web 8080), web panel, PyVISA demo |
| `tui` | Terminal UI |
| `prototype/pot-adc` | First version: one potentiometer, Zig firmware + its own TUI |
| `tests` | Parser unit tests + fuzzing (on the PC) |
| `tools` | On-board tests, `scpi.py` command-line client |

## Tests

```sh
make test          # parser: unit tests + 2 M fuzz tokens under ASan / UBSan
make board-test    # on the board; stop the bridge first
make dvm-test      # voltmeter, nothing needs to be wired
.venv/bin/python tools/lab_test.py    # generator, scope, measurements
.venv/bin/python tools/bode_test.py   # needs the RC on PA4 / A0
```

CI (GitHub Actions) runs the parser tests and builds the firmware, both TUIs and the Zig prototype on every push. The firmware `.bin` is kept as a build artifact.

## Talking to it by hand

```sh
tools/scpi.py '*IDN?' 'MEAS:VOLT? (@1:6)'
tools/scpi.py              # interactive
tools/scpi.py -e 'CALC:LIM:UPP 9'    # also show SYST:ERR?
```

It uses the bridge when it is running, else the serial port. Only one program can hold the serial port at a time.

## The Zig prototype

`prototype/pot-adc` is where the project started: one potentiometer on A0, register-level Zig with no libraries. ADC1 averages 16 samples, VREFINT gives the real supply, and each reading goes out over USART2 as `P <raw> <mV> <vdd_mV>` at 115200 baud. `tui/` draws it with hnoss, and `watch.sh` reads the same values through OpenOCD without the serial port.

```sh
cd prototype/pot-adc && zig build flash
cd tui && odin run .
```

Flashing it replaces NucleoLab; `make flash` at the top level puts NucleoLab back.

## Limits

- The DAC and ADC share one reference and clock, so measuring yourself doesn't prove absolute accuracy.
- No anti-alias filter: signals above half the sample rate show up at the wrong frequency.
- No smoothing filter on the DAC output. When an ADC sample lands on a DAC step, SNR drops (10 kHz at 1 MS/s: median 62 dB, worst 44 dB).
- A Bode sweep switches the output to a 2 Vpp sine and restores it afterwards.
- The bridge listens on 127.0.0.1 only and has no authentication.

`make restore` writes back the board's previous firmware from `backup/original_flash.bin`. That file isn't in the repo: dump your own first with `st-flash read backup/original_flash.bin 0x08000000 0x80000`.
