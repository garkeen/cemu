/* Layout for the v86 tests/nasm sources used here.
 *
 * cemu enters a multiboot image in flat protected mode with EAX = 0x2badb002
 * (src/cpu/isa/x86/exec.c's x86_init), and finds the header by scanning the
 * first 0x2000 bytes of the loaded image, so the header has to be inside that
 * window. The Multiboot 0.6.96 spec (§3.1.1) additionally requires it inside
 * the first 8192 bytes of the *file*, which a section at 0x1000 satisfies
 * (the ELF headers occupy 0x100..0x100).
 *
 * .bss then sits at 0x100000: the suite's own header.inc puts `stack_top` at
 * the end of an 8 KiB .bss, i.e. 0x102000, and its fixture window
 * (v86 tests/nasm/run.js BSS/WINDOW) is exactly 0x100000..0x102000 — the
 * scratch area those tests measure after the last instruction.
 */
ENTRY(_start)
SECTIONS
{
  . = 0x1000;
  .multiboot : { *(.multiboot) }
  . = 0x2000;
  .text : { *(.text) *(.rodata) *(.data) }
  . = 0x100000;
  .bss : { *(.bss) *(COMMON) }
}
