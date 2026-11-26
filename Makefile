CC := arm-none-eabi-gcc
T  := third_party
CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
INC := -Ifw/scpi -Ifw/board -I$(T)/cmsis_device_l4/Include -I$(T)/cmsis_core/CMSIS/Core/Include
CFLAGS := $(CPU) $(INC) -DSTM32L452xx -std=c11 -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections
SRC := $(wildcard fw/*/*.c) $(T)/cmsis_device_l4/Source/Templates/gcc/startup_stm32l452xx.s
HDR := $(wildcard fw/*/*.h)

all: build/nucleolab.bin
build/nucleolab.elf: $(SRC) $(HDR) fw/l452re.ld Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -T fw/l452re.ld -nostartfiles --specs=nano.specs -Wl,--gc-sections -Wl,-Map=build/nucleolab.map -lm -o $@
	arm-none-eabi-size $@
build/nucleolab.bin: build/nucleolab.elf
	arm-none-eabi-objcopy -O binary $< $@
flash: build/nucleolab.bin
	st-flash --connect-under-reset --reset write $< 0x08000000
restore:
	st-flash --connect-under-reset --reset write backup/original_flash.bin 0x08000000
test:
	@mkdir -p build
	gcc -std=c11 -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Ifw/scpi tests/test_scpi.c fw/scpi/scpi.c -o build/test_scpi
	./build/test_scpi
board-test:
	.venv/bin/python tools/board_test.py
tui:
	cd tui && odin build . -vet -strict-style -o:speed -out:nucleolab-tui
clean:
	rm -rf build tui/nucleolab-tui
.PHONY: all flash restore test board-test tui clean
