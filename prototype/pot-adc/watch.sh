#!/bin/sh
# Reads the pot values from the running board every ~second (Ctrl+C to stop).
# Usage: ./watch.sh [count]
cd "$(dirname "$0")"
ELF=zig-out/bin/pot-adc.elf
addr() { arm-none-eabi-nm "$ELF" | awk -v s="$1" '$3==s {print "0x"$1}'; }
RAW=$(addr dbg_pot_raw); MV=$(addr dbg_pot_mv); VDD=$(addr dbg_vdd_mv); PCT=$(addr dbg_percent)
n=${1:-0}; i=0
while [ "$n" -eq 0 ] || [ "$i" -lt "$n" ]; do
  out=$(openocd -f interface/stlink.cfg -f target/stm32l4x.cfg -c "reset_config none" -c init -c halt \
    -c "mdw $RAW" -c "mdw $MV" -c "mdw $VDD" -c "mdw $PCT" -c resume -c shutdown 2>&1 | grep '^0x' | awk '{print $2}')
  set -- $out
  printf "raw=%4d  pot=%4d mV  vdd=%4d mV  %3d%%\n" 0x$1 0x$2 0x$3 0x$4
  i=$((i+1))
done
