OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
  . = 0x80000000;
  .text : { *(.text.init) *(.text) }
  . = 0x80001000;
  .tohost : { *(.tohost) }
}
