# Voltmeter and plug detection

Six DC channels, one per Arduino analog pin: channel *n* is A(*n*−1). Pin mapping: [hardware.md](hardware.md).

```
$ tools/scpi.py 'ROUT:CHAN:CAT?' 'MEAS:VOLT? (@1:6)'
(@2)
9.91E+37,1.652337E+00,9.91E+37,9.91E+37,9.91E+37,9.91E+37
```

## How the board knows a pin is empty

An unconnected ADC input doesn't read 0 V: it floats wherever leakage and the last conversion left it. Waiting for a reading to "look random" is slow and unreliable. Instead, the board asks the pin directly:

1. Switch the pin to input with the weak internal **pull-up** (~40 kΩ). This charges the pin's few pF.
2. Switch back to analog, wait 5 µs, convert.
3. Do the same with the **pull-down**.

A floating pin keeps that charge, so the two readings land ~800 codes apart. A pin that something drives reads its own voltage both times (~0 apart). Each step repeats four times, so the ADC's own sampling capacitor is charged too and doesn't pull the reading back.

| | probe spread |
|---|---|
| potentiometer wiper, any position | 0 ± 6 codes |
| wire to GND or 3V3 | 0 ± 2 |
| nothing connected | 720 – 840 |

The threshold is 300 codes (`ROUT:CHAN:DET:THR`). A change has to show twice in a row before it counts. One channel is probed every 20 ms, so a plug or unplug is noticed within ~120–240 ms.

The pulls are only on for ~200 µs per probe, through 40 kΩ: a few tens of µA at most, harmless for anything you'd measure. When even that is too much, `ROUT:CHAN:DET OFF` stops probing entirely and treats every channel as connected.

## Background sampling

On its turn, a connected channel is also sampled (16 averages). This feeds:

- **Statistics** since the channel was plugged in: `CALC:AVER:MIN?`, `MAX?`, `AVER?`, `COUN?`, cleared with `CALC:AVER:CLE`.
- **Limits**: `CALC:LIM:LOW 0.5,(@1)`, `CALC:LIM:UPP 2.5,(@1)`, `CALC:LIM ON`. `CALC:LIM:FAIL?` reports per channel. While any channel is out, `STAT:QUES:COND` bit 10 is set, and its event bit latches, so `*STB?` can tell you without polling values.
- **Event log**: plug, unplug and limit changes, timestamped by the board.

```
ROUT:CHAN:EVEN?        → 2,2,117304     CH2 went out of limits at 117.304 s
ROUT:CHAN:EVEN?        → 2,3,119420     ...and came back
ROUT:CHAN:EVEN?        → 0,0,0          log empty
```

`what` is 0 unplugged, 1 plugged in, 2 out of limits, 3 back within limits. The log holds 16 events; when full, the oldest goes.

## Measurement

`MEAS:VOLT? (@list)` converts fresh, with `VOLT:AVER:COUN` samples per channel (1–256, default 64) at the longest sample time (640.5 ADC cycles), so source impedances up to ~50 kΩ settle. `MEAS:VOLT:RAW?` returns the same average in ADC codes (0–4095) before scaling.

Each sample is scaled with VDDA measured through VREFINT:

```
volts = code × VDDA / 4096,   VDDA = 3.0 V × VREFINT_CAL / VREFINT_code
```

## Commands

| Command | Reply / effect |
|---|---|
| `MEAS:VOLT? (@list)` | volts; `9.91E+37` (SCPI NaN) where nothing is connected |
| `MEAS:VOLT:RAW? (@list)` | averaged ADC codes |
| `[SENS:]VOLT:AVER:COUN <1–256>` | samples per `MEAS:VOLT?` |
| `ROUT:CHAN:CONN? [(@list)]` | 1 / 0 per channel |
| `ROUT:CHAN:CAT?` | connected channels, e.g. `(@1,4)` |
| `ROUT:CHAN:NAME? [(@list)]` | `"A0","A1",…` |
| `ROUT:CHAN:CHAN?` | plug / unplug count since boot |
| `ROUT:CHAN:EVEN?` / `:COUN?` | oldest event / events waiting |
| `ROUT:CHAN:DET[:STAT] ON\|OFF` | plug detection |
| `ROUT:CHAN:DET:THR <50–4000>` | probe spread that means "floating" |
| `CALC:AVER:MIN? \| MAX? \| AVER? \| COUN? [(@list)]` | statistics since plug-in |
| `CALC:AVER:CLE [(@list)]` | restart statistics |
| `CALC:LIM:LOW \| UPP <V>[,(@list)]` | limits (query with `?`) |
| `CALC:LIM[:STAT] ON\|OFF` | limit checking |
| `CALC:LIM:FAIL? [(@list)]` | 1 where out of limits |
| `DIAG:CHAN:PROB? [(@list)]` | raw probe spread, for tuning |

A list is `(@1)`, `(@1,3)`, `(@2:5)` or any mix like `(@1,3:5)`. Without one, a query covers all six channels. `*RST` restores the defaults: 64 averages, detection on, threshold 300, limits off at 0–3.6 V.
