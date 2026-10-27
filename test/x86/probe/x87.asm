; x87 FPU probe (self-contained multiboot ELF, no devices/BIOS).
; Covers the register stack, arithmetic through Berkeley SoftFloat, the
; control/status words, integer/BCD conversion, FXSAVE/FXRSTOR and the
; CR0.TS -> #NM path. Pass exits through isa-debug-exit 0xF4 with payload 5
; (status 11); a failed check exits with a per-check payload (status > 1).
;
; Build: nasm -f elf32 -o x87.o x87.asm
;        ld.lld -m elf_i386 -T ../pm/pm_smoke.ld -o x87.elf x87.o
; Run:   timeout 30 cemu --machine x86 --isa x86 x87.elf

bits 32

%define GDT     0x2000
%define GDTPTR  0x1f00
%define IDT     0x3000
%define IDTPTR  0x2f00
%define B       0x80000
%define B80     0x80010
%define FX      0x81000
%define M       0x80400
%define NMFLAG  0x80404

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

  ; IDT: #NM (7) has its own handler that clears TS; #MF (16), #UD (6),
  ; #GP (13), #PF (14) record and fail.
  mov word [IDTPTR], 0x7ff
  mov dword [IDTPTR + 2], IDT
  xor eax, eax
  mov ecx, 0x400
  mov edi, IDT
  rep stosd
  mov eax, nm_handler
  call set_gate7
  mov eax, mf_handler
  call set_gate16
  mov eax, bad_handler
  call set_gate6
  mov eax, bad_handler
  call set_gate13
  mov eax, bad_handler
  call set_gate14
  lidt [IDTPTR]

  fninit
  fnstcw [B]
  cmp word [B], 0x037f
  jne fail1
  fnstsw ax
  cmp ax, 0
  jne fail1

  ; 1 + 1 = 2, through the stack and a double store.
  fld1
  fld1
  faddp st1, st0
  fstp qword [B]
  cmp dword [B], 0
  jne fail2
  cmp dword [B + 4], 0x40000000
  jne fail2

  ; pi, correctly rounded to double.
  fldpi
  fstp qword [B]
  cmp dword [B], 0x54442d18
  jne fail3
  cmp dword [B + 4], 0x400921fb
  jne fail3

  ; sqrt(2).
  fld1
  fld1
  faddp st1, st0
  fsqrt
  fstp qword [B]
  cmp dword [B], 0x667f3bcd
  jne fail4
  cmp dword [B + 4], 0x3ff6a09e
  jne fail4

  ; 80-bit store keeps pi's 64-bit significand.
  fldpi
  fstp tword [B80]
  cmp dword [B80], 0x2168c235
  jne fail5
  cmp dword [B80 + 4], 0xc90fdaa2
  jne fail5
  cmp word [B80 + 8], 0x4000
  jne fail5

  ; FCOMI: 0 < 1 -> CF=1, ZF=0, PF=0.
  fld1
  fldz
  fcomip st1
  pushfd
  pop eax
  test eax, 1
  jz fail6
  test eax, 0x40
  jnz fail6
  test eax, 4
  jnz fail6

  ; Integer round trip.
  mov dword [B], -12345
  fild dword [B]
  fistp dword [B]
  cmp dword [B], -12345
  jne fail7

  ; RC=truncate (CW = 0x0f7f): 2.7 -> 2.
  mov ax, 0x0f7f
  mov [B], ax
  fldcw [B]
  mov dword [B], 0x402ccccd  ; 2.7f
  fld dword [B]
  fistp dword [B]
  cmp dword [B], 2
  jne fail8
  mov ax, 0x037f
  mov [B], ax
  fldcw [B]

  ; FXSAVE/FXRSTOR round trip of the x87 stack (the state images require
  ; CR4.OSFXSR).
  mov eax, cr4
  or eax, 0x600        ; OSFXSR | OSXMMEXCPT
  mov cr4, eax
  fldpi
  fxsave [FX]
  fninit
  fxrstor [FX]
  fstp qword [B]
  cmp dword [B], 0x54442d18
  jne fail9
  cmp dword [B + 4], 0x400921fb
  jne fail9

  ; MXCSR reset value and read-back.
  stmxcsr [B]
  cmp dword [B], 0x1f80
  jne fail10

  ; CR0.TS: the escape must raise #NM; the handler clears TS and the
  ; instruction re-executes.
  mov byte [NMFLAG], 0
  mov eax, cr0
  or eax, 8
  mov cr0, eax
  fld1
  fstp st0
  cmp byte [NMFLAG], 1
  jne fail11

  ; Stack overflow/underflow leave the architectural indefinite result.
  fninit
  mov ecx, 9
.ovf:
  fld1
  dec ecx
  jnz .ovf
  fnstsw ax
  test ax, 0x41          ; IE | SF
  jz fail12

  mov al, 5
  out 0xf4, al
  hlt

set_gate7:
  mov [IDT + 7 * 8], ax
  mov word [IDT + 7 * 8 + 2], 0x08
  mov byte [IDT + 7 * 8 + 4], 0
  mov byte [IDT + 7 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + 7 * 8 + 6], ax
  ret
set_gate16:
  mov [IDT + 16 * 8], ax
  mov word [IDT + 16 * 8 + 2], 0x08
  mov byte [IDT + 16 * 8 + 4], 0
  mov byte [IDT + 16 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + 16 * 8 + 6], ax
  ret
set_gate6:
  mov [IDT + 6 * 8], ax
  mov word [IDT + 6 * 8 + 2], 0x08
  mov byte [IDT + 6 * 8 + 4], 0
  mov byte [IDT + 6 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + 6 * 8 + 6], ax
  ret
set_gate13:
  mov [IDT + 13 * 8], ax
  mov word [IDT + 13 * 8 + 2], 0x08
  mov byte [IDT + 13 * 8 + 4], 0
  mov byte [IDT + 13 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + 13 * 8 + 6], ax
  ret
set_gate14:
  mov [IDT + 14 * 8], ax
  mov word [IDT + 14 * 8 + 2], 0x08
  mov byte [IDT + 14 * 8 + 4], 0
  mov byte [IDT + 14 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + 14 * 8 + 6], ax
  ret

nm_handler:
  clts
  mov byte [NMFLAG], 1
  iret

mf_handler:
  mov al, 0x0e
  out 0xf4, al
  hlt

bad_handler:
  mov al, 0x0f
  out 0xf4, al
  hlt

fail1:  mov al, 1
        jmp fail
fail2:  mov al, 2
        jmp fail
fail3:  mov al, 3
        jmp fail
fail4:  mov al, 4
        jmp fail
fail5:  mov al, 6
        jmp fail
fail6:  mov al, 7
        jmp fail
fail7:  mov al, 8
        jmp fail
fail8:  mov al, 9
        jmp fail
fail9:  mov al, 10
        jmp fail
fail10: mov al, 12
        jmp fail
fail11: mov al, 13
        jmp fail
fail12: mov al, 14
fail:
  out 0xf4, al
  hlt

