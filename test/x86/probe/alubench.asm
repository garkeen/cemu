; Register-only loop: measures per-instruction overhead (decode, setjmp,
; step bookkeeping) with no memory accesses. Exit through 0xF4 payload 5.
bits 32

%define N 10000000

section .multiboot
align 4
  dd 0x1BADB002
  dd 0x3
  dd -(0x1BADB002 + 0x3)

section .text
global _start
_start:
  cli
  mov ecx, N
  xor eax, eax
.loop:
  add eax, ecx
  xor edx, edx
  inc edx
  dec ecx
  jnz .loop
  mov al, 5
  mov dx, 0xf4
  out dx, al
.hang:
  hlt
  jmp .hang
