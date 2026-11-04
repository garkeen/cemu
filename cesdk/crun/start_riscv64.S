// riscv64 entry: cemu's -bios loads the ELF at 0x80000000 and starts in
// M-mode at e_entry.  No firmware runs before this, so zero .bss, put the
// stack where the linker script reserved it, and call main.

    .section .text.start, "ax", @progbits
    .globl _start
_start:
    la      t0, __bss_start
    la      t1, __bss_end
1:
    bgeu    t0, t1, 2f
    sb      zero, 0(t0)
    addi    t0, t0, 1
    j       1b
2:
    la      sp, __stack_top
    call    main
    call    Halt
3:
    j       3b
