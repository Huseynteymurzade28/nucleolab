# Terminal UI and web panel

## TUI

```sh
make tui && tui/nucleolab-tui [/dev/ttyACMx]
```

Written in Odin with [hnoss](https://github.com/Huseynteymurzade28/hnoss). It connects to the bridge on `127.0.0.1:5025` when `./run.sh` is running, so the web panel keeps working alongside. Otherwise it opens the serial port itself; start the bridge first if you want both. It reconnects on its own when the board is reset or unplugged.

| Key | |
|---|---|
| `Enter` | send the console line as a SCPI command |
| `↑` `↓` | command history |
| `Tab` / `Shift+Tab` | select channel |
| `PgUp` / `PgDn` | longer / shorter graph time span (×1 – ×64) |
| `F1` | help |
| `F2` | start / stop CSV recording |
| `Esc` | close help · quit |

**Channels**: state, voltage, a gauge and a sparkline per input. A channel outside its limits turns red.

**Selected channel**: the reading in big digits, min / max / span since plug-in, its limits when they're on, and a braille graph of the history. The frame shows the zoom and the time it covers.

**Console**: anything typed goes to the board, followed by `SYST:ERR?`, so every command shows its reply and any error. Plug, unplug and limit events from the board's log appear here with the board's own timestamp.

**CSV**: `F2` writes `nucleolab-<unix time>.csv` in the current directory, one row per reading:

```
time_s,ch1_A0_V,ch2_A1_V,ch3_A2_V,ch4_A3_V,ch5_A4_V,ch6_A5_V,vdda_V
0.049,,1.6519,,,,,3.3024
```

### How it talks to the board

Polls go out ~20 times a second as one line: `MEAS:VOLT? (@1:6);:SYST:VDDA?;:CALC:LIM:FAIL?;:ROUT:CHAN:EVEN?`. Limits are re-read once a second. Every transaction ends with `:SYST:SYNC? <n>`, and the reply is complete when `SYNC,<n>` comes back. That works the same over serial and through the bridge, and commands without a reply never leave it waiting. On connect it sends `*CLS`, so errors left over from an earlier session don't show up as the reply to your first command.

## Web panel

```sh
./run.sh     # http://127.0.0.1:8080
```

Scope, spectrum and Bode views with cursors, CSV export and a SCPI console with tab completion. The generator, scope and Bode settings are on the right. The page is a single file, `host/panel.html`, served by `host/server.py`. The UI text is in Turkish.

When a PyVISA script (`host/demo_pyvisa.py`) talks to port 5025, the panel switches to "remote control" and stops polling until it's done.
