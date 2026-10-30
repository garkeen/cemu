; MOV r/m16,Sreg (0x8C) and MOV Sreg,r/m16 (0x8E) encoding semantics.
;
; Pins three rules the SDM states (MOV page, Operation/Notes) and QEMU's
; translate.c implements:
;   1. 0x8C with a MEMORY destination stores exactly 16 bits whatever the
;      operand size; the 32-bit form only zero-extends a REGISTER destination.
;   2. `mov eax, ds` writes 32 bits with the upper half zero.
;   3. 0x8C and 0x8E with reg=6/7 are #UD (reserved segment encodings).
; The scratch area is pre-filled with 0xa5 so a 32-bit store into a 16-bit
; slot shows up as two zero bytes.
;
; Build: nasm -f elf32 -o movsreg.o movsreg.asm
;        ld.lld -m elf_i386 -T ../pm/pm_smoke.ld -o movsreg.elf movsreg.o
; Run:   cemu --machine x86 --isa x86 movsreg.elf
;        qemu-system-i386 -machine pc -m 32 -kernel movsreg.elf -display none
;            -no-reboot -serial file:movsreg_qemu.txt
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit 5 (status 11 on both).

bits 32

%define GDT     0x2000
%define GDTPTR  0x1f00
%define IDT     0x3000
%define IDTPTR  0x2f00
%define B       0x80000
%define DBGEXIT 0xf4

section .multiboot
align 4
  dd 0x1BADB002
  dd 0x3
  dd -(0x1BADB002 + 0x3)

section .text
global _start
_start:
  cli
  mov dword [GDT], 0
  mov dword [GDT + 4], 0
  mov dword [GDT + 8], 0x0000ffff
  mov dword [GDT + 12], 0x00cf9b00
  mov dword [GDT + 16], 0x0000ffff
  mov dword [GDT + 20], 0x00cf9300
  mov word [GDTPTR], 23
  mov dword [GDTPTR + 2], GDT
  lgdt [GDTPTR]
  mov ax, 0x10
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  mov ss, ax
  jmp 0x08:.cs
.cs:

  ; IDT: all zero except vector 6 (#UD).
  mov word [IDTPTR], 0x7ff
  mov dword [IDTPTR + 2], IDT
  xor eax, eax
  mov ecx, 0x400
  mov edi, IDT
  rep stosd
  mov eax, isr6
  mov ebx, 6
  call set_gate
  lidt [IDTPTR]

  call uart_init

  ; Scratch filled with 0xa5.
  mov edi, B + 0x100
  mov ecx, 0x40
  mov al, 0xa5
  rep stosb

  ; 1. 32-bit operand size, memory destination: only 2 bytes may be written.
  db 0x8c, 0x1d
  dd B + 0x100
  mov eax, [B + 0x100]
  mov ebx, [B + 0x104]
  mov esi, s_w32
  call report

  ; 2. 16-bit operand size control (66 prefix).
  db 0x66, 0x8c, 0x1d
  dd B + 0x110
  mov eax, [B + 0x110]
  mov ebx, [B + 0x114]
  mov esi, s_w16
  call report

  ; 3. register destination zero-extends: `mov eax, ds` -> 0x00000010.
  db 0x8c, 0xd8
  mov ebx, eax
  mov esi, s_rex
  call report

  ; 4. 0x8C /6 and /7 are #UD; the handler skips the 6-byte instruction.
  mov byte [B + 0x1f1], 0
  db 0x8c, 0x35
  dd B + 0x120
  movzx eax, byte [B + 0x1f1]
  mov esi, s_t6
  call report

  mov byte [B + 0x1f1], 0
  db 0x8c, 0x3d
  dd B + 0x124
  movzx eax, byte [B + 0x1f1]
  mov esi, s_t7
  call report

  ; 5. 0x8E /6 is #UD too (control: this one is already checked).
  mov byte [B + 0x1f1], 0
  db 0x8e, 0x35
  dd B + 0x130
  movzx eax, byte [B + 0x1f1]
  mov esi, s_t8e
  call report

  call crlf
  mov al, 5
  mov dx, DBGEXIT
  out dx, al
  hlt
  jmp $

; eax, ebx: two words printed after the label in esi.
report:
  push eax
  push ebx
  call puts
  mov al, '='
  call putc
  pop ebx
  pop eax
  push eax
  push ebx
  call puthex32
  mov al, ','
  call putc
  pop ebx
  mov eax, ebx
  call puthex32
  pop eax
  mov al, ' '
  call putc
  ret

set_gate:
  push eax
  push ebx
  push ecx
  mov ecx, ebx
  shl ecx, 3
  mov word [IDT + ecx], ax
  mov word [IDT + ecx + 2], 0x08
  mov byte [IDT + ecx + 4], 0
  mov byte [IDT + ecx + 5], 0x8e
  shr eax, 16
  mov word [IDT + ecx + 6], ax
  pop ecx
  pop ebx
  pop eax
  ret

isr6:
  mov byte [B + 0x1f1], 1
  add dword [esp], 6
  iretd

puts:
  push eax
  push esi
.loop:
  lodsb
  test al, al
  jz .done
  call putc
  jmp .loop
.done:
  pop esi
  pop eax
  ret

puthex32:
  push eax
  push ecx
  mov ecx, 8
.loop:
  rol eax, 4
  mov edx, eax
  and edx, 0x0f
  push eax
  mov al, dl
  cmp al, 10
  jb .digit
  add al, 'a' - 10
  jmp .emit
.digit:
  add al, '0'
.emit:
  call putc
  pop eax
  dec ecx
  jnz .loop
  pop ecx
  pop eax
  ret

putc:
  push eax
  push edx
  mov dx, 0x3fd
.wait:
  in al, dx
  test al, 0x20
  jz .wait
  pop edx
  pop eax
  mov dx, 0x3f8
  out dx, al
  ret

crlf:
  mov al, 13
  call putc
  mov al, 10
  jmp putc

uart_init:
  mov dx, 0x3fb
  mov al, 0x80
  out dx, al
  mov dx, 0x3f8
  mov al, 0x01
  out dx, al
  mov dx, 0x3f9
  xor al, al
  out dx, al
  mov dx, 0x3fb
  mov al, 0x03
  out dx, al
  mov dx, 0x3f9
  xor al, al
  out dx, al
  ret

section .rodata
s_w32: db "w32", 0
s_w16: db "w16", 0
s_rex: db "rex", 0
s_t6:  db "t6", 0
s_t7:  db "t7", 0
s_t8e: db "t8e", 0
