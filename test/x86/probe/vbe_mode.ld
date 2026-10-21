/* Flat protected-mode image at 64 KiB: same shape as vga_core.ld. */
OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
SECTIONS {
  . = 0x10000;
  .text : { *(.multiboot) *(.text*) *(.rodata*) *(.data*) }
  .bss  : { *(.bss*) }
}
