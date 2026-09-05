/* Flat protected-mode image at 64 KiB: the multiboot header must sit at the
   image start (cemu scans RAM from the load base, QEMU scans the first 8 KiB
   of the file for -kernel multiboot). */
OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
SECTIONS {
  . = 0x10000;
  .text : { *(.multiboot) *(.text*) *(.rodata*) *(.data*) }
  .bss  : { *(.bss*) }
}
