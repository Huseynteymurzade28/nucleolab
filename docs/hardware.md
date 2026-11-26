# Hardware

NucleoLab targets the **NUCLEO-L452RE-P** (MB1319, STM32L452RET6P, Cortex-M4F at 80 MHz). The ST-LINK on the board flashes it and gives a serial port for SCPI.

## Pins

| Function | Header | MCU pin | Notes |
|---|---|---|---|
| Voltmeter CH1 | A0 | PA0 | also scope channel 1 |
| Voltmeter CH2 | A1 | PA1 | also scope channel 2 |
| Voltmeter CH3 | A2 | PC3 | |
| Voltmeter CH4 | A3 | PC2 | |
| Voltmeter CH5 | A4 | PC1 | |
| Voltmeter CH6 | A5 | PC0 | |
| Generator out | morpho | PA4 | DAC1; scope channel 3 reads it internally |
| User LED | – | PB13 | LD4, `OUTP:LED` |
| User button | – | PC13 | B1, `DIG:INP?` |
| SCPI serial | – | PA2 / PA3 | USART2 → ST-LINK, 921600 8N1 |

**Inputs take 0–3.3 V only.** Never connect PA4 to another output.

### Plain NUCLEO-L452RE

The non-P board (MB1136) routes A2 to PA4 and A3 to PB0, and has its LED on PA5. To run there, edit `chmap` in `fw/board/analog.c` and `LED_PIN` in `fw/board/board.c`.

How to tell them apart: the -P has "SMPS" on the silkscreen and an extra SMPS circuit near the MCU. On the -P, the Arduino I²C pin D14 goes to PB7 (PB9 on the plain board).

> [!WARNING]
> On the STM32L452RE**xP**, PC5 and PD2 are not GPIOs: those balls carry the external SMPS core supply (VDD12). Writing their GPIO registers corrupts instruction fetches until the next power cycle. The firmware never touches them.

## Wiring

**Potentiometer** (any 1–50 kΩ): outer legs to 3V3 and GND, the wiper to any of A0–A5. It shows up within ~200 ms.

**Scope self-test**: a jumper from PA4 to A0, then scope channel 1. Without it, use channel 3 (PA4 internally).

**Bode, RC low-pass**: PA4 → 10 kΩ → A0, A0 → 10 nF → GND. Theory: −3 dB at 1.59 kHz.
Keep the load at 5 kΩ or more: the DAC output buffer can't drive less.

## Power

The ST-LINK reports the target voltage (`openocd` prints it on connect), and `SYST:VDDA?` measures it from the MCU side through VREFINT and its factory calibration. Both should read about 3.3 V. Everything the voltmeter reports is scaled with the measured VDDA, not an assumed 3.3 V.
