// i386 entry for the multiboot contract cemu implements (cpu/isa/x86/exec.c
// x86_init): the image is loaded at p_vaddr, EAX = 0x2BADB002, and CS=0x08 /
// DS..GS=0x10 are flat 4 GiB descriptors with D/B set.  The header below must
// sit in the first 8 KiB of the loaded image.

    .section .multiboot, "a", @progbits
    .align 4
    .long 0x1BADB002            // magic
    .long 0x00000000            // flags: no info requests; we only need entry
    .long -(0x1BADB002 + 0)     // checksum

    .section .text.start, "ax", @progbits
    .globl _start
_start:
    movl    $__bss_start, %edi
    movl    $__bss_end, %ecx
    subl    %edi, %ecx
    xorl    %eax, %eax
    rep stosb
    movl    $__stack_top, %esp
    call    main
    pushl   %eax
    call    Halt
1:
    jmp     1b
