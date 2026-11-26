# SCPI reference

The board speaks SCPI / IEEE 488.2 over the ST-LINK serial port (921600 8N1, `\n`-terminated). The bridge (`./run.sh`) also exposes it as a raw socket on `127.0.0.1:5025`, so PyVISA works with `TCPIP::127.0.0.1::5025::SOCKET`.

Headers take the short form (upper-case letters) or the long form in any case: `MEAS:VOLT?` = `measure:voltage?`. Optional nodes are in `[ ]`. Several commands go on one line with `;`, and a leading `:` returns to the root. Numbers take units and SI prefixes (`250 mV`, `2 kHz`) and `MIN` / `MAX` / `DEF`.

## Common

`*IDN?` `*RST` `*CLS` `*TST?` `*OPC` `*OPC?` `*WAI` `*ESE[?]` `*ESR?` `*SRE[?]` `*STB?`

`*TST?` returns a bit mask: 1 not on PLL, 2 clock not crystal-locked, 4 VDDA out of range, 8 DAC → ADC loop error. 0 = pass.

## Generator (DAC on PA4)

| Command | Range |
|---|---|
| `SOUR:FUNC[:SHAP] SIN\|SQU\|TRI\|RAMP\|DC\|ARB` | |
| `SOUR:FREQ <Hz>` | 1 Hz – 100 kHz; `SOUR:FREQ?` returns the frequency actually produced |
| `SOUR:VOLT[:AMPL] <Vpp>` | 0 – 3.6, must fit with the offset |
| `SOUR:VOLT:OFFS <V>` | 0 – 3.6 |
| `SOUR:FUNC:DCYC <%>` | 1 – 99, square only |
| `SOUR:DATA:ARB <block>` | 2 – 4096 points, uint16 little endian, 0 – 4095 |
| `OUTP[:STAT] ON\|OFF` | |

## Scope

| Command | Range |
|---|---|
| `ACQ:CHAN <1–3>` | 1 PA0, 2 PA1, 3 PA4 (internal) |
| `ACQ:SRAT <Hz>` | 1 kHz – 1 MHz |
| `ACQ:POIN <n>` | 16 – 8192 |
| `TRIG:LEV <V>`, `TRIG:SLOP POS\|NEG`, `TRIG:SWE AUTO\|NORM`, `TRIG:POS <%>` | |
| `INIT`, `ABOR`, `TRIG:FORC`, `ACQ:STAT?` | `IDLE`, `ARM`, `DONE`, `ERR` |
| `WAV:PRE?` | points, s/sample, V/code, trigger index, real trigger (0/1) |
| `WAV:DATA?` | block of uint16 little endian codes |

## Measurements

Each of these takes a fresh capture with the current `ACQ` settings:
`MEAS:VOLT[:DC]?` `MEAS:VOLT:AC?` `:RMS?` `:MIN?` `:MAX?` `:PTP?` `MEAS:FREQ?`

With a channel list, `MEAS:VOLT? (@list)` is the voltmeter instead: see [voltmeter.md](voltmeter.md).

`MEAS:TEMP?` returns the die temperature, and `SYST:VDDA?` the supply.

## Analysis

| Command | Reply |
|---|---|
| `CALC:SPEC?` | spectrum of the last capture: block of int16 LE, centi-dBV per bin |
| `CALC:DIST?` | bins, bin width Hz, fundamental Hz, fundamental Vrms, THD dB, SNR dB, SINAD dB |
| `BODE:POIN? <Hz>[,<ch>]` | Hz, gain dB, phase °, reference Vpp |

## Voltmeter

See [voltmeter.md](voltmeter.md#commands).

## System and status

| Command | |
|---|---|
| `SYST:ERR[:NEXT]?`, `SYST:ERR:COUN?` | error queue, 16 deep |
| `SYST:VERS?`, `SYST:HELP:HEAD?` | SCPI version, every header the board knows |
| `SYST:UPT?`, `SYST:CLOC?` | uptime in s, core clock in Hz |
| `SYST:SYNC? <n>` | replies `SYNC,<n>`: marks the end of a reply stream |
| `OUTP:LED[:STAT] ON\|OFF`, `DIG:INP?` | user LED, user button |
| `DIAG:CAL?` | VREFINT and temperature calibration words |
| `DIAG:LOOP?` | worst DAC → ADC error, via jumper and internal |
| `STAT:OPER[:EVEN]? \| :COND? \| :ENAB` | bit 4 acquisition running, bit 9 a channel was plugged / unplugged |
| `STAT:QUES[:EVEN]? \| :COND? \| :ENAB` | bit 10 a channel is out of its limits |
| `STAT:PRES` | clear the enable masks |

## Errors

| Code | Meaning |
|---|---|
| −101 −102 −104 | invalid character, syntax, wrong data type |
| −108 −109 | too many, too few parameters |
| −113 −114 | unknown header, header suffix out of range |
| −131 −138 | bad unit, unit not allowed |
| −151 −160 −161 | broken string, broken binary block |
| −211 −213 −214 | trigger ignored, init ignored, trigger deadlock |
| −221 | settings conflict (e.g. ADC busy with a capture) |
| −222 −224 | out of range, illegal value |
| −230 −240 | no data yet, hardware error |
| −350 −363 | error queue overflow, input buffer overrun |
