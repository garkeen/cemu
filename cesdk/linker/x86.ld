OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS {
  . = 1M;

  .text : {
    *(.multiboot)
    *(.text.start)
    *(.text*)
  }

  .rodata : { *(.rodata*) }
  .data   : { *(.data*) }

  .bss : {
    __bss_start = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(16);
    __stack_bottom = .;
    . += 0x4000;
    __stack_top = .;
    __bss_end = .;
  }
}
