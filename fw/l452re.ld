ENTRY(Reset_Handler)
MEMORY {
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
  RAM2  (rwx) : ORIGIN = 0x10000000, LENGTH = 32K
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
  .isr_vector : { KEEP(*(.isr_vector)) . = ALIGN(8); } > FLASH
  .text : { *(.text*) *(.rodata*) . = ALIGN(8); } > FLASH
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH
  .init_array : { __preinit_array_start = .; KEEP(*(.preinit_array*)) __preinit_array_end = .;
                  __init_array_start = .; KEEP(*(.init_array*)) __init_array_end = .; } > FLASH
  _sidata = LOADADDR(.data);
  .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT > FLASH
  .bss (NOLOAD) : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
  .sram2 (NOLOAD) : { *(.sram2*) } > RAM2
  ASSERT(_ebss + 8K <= _estack, "less than 8K left for the stack")
}
