; x87 pending-exception probe (self-contained multiboot ELF).
;
; Checks the parts of SDM vol.1 8.7.1 / 8.3.12 that decide when #MF arrives and
; what the non-waiting control instructions do with a pending exception:
;   t1  0/0 with IE unmasked sets IE, ES and B, but does not raise #MF at the
;       faulting instruction
;   t2  FNSTSW AX (non-waiting) executes with that exception pending
;   t3  the next waiting x87 instruction (FLD1) raises #MF, and the saved CS:EIP
;       names FLD1, not the instruction that set the flag
;   t4  FNSTENV (non-waiting) stores the old CW and then masks all exceptions;
;       FNSTCW reads back CW|0x3F, ES and B are clear, IE stays set, and a
;       following FLD1 does not raise #MF
;
; Exit: pass payload 5 (status 11); failure payload 1..12 (status 3..25).
;
; QEMU 11.1.50 diverges on t3: its TCG checks pending x87 exceptions only at
; WAIT/FWAIT (translate.c sets update_fip=false for FNOP and calls
; helper_fwait), so it never raises #MF at the FLD1 and the probe stops at
; fail3 (status 9). SDM vol.1 8.7.1 names "the next floating-point
; instruction, unless it is one of the non-waiting instructions" as a check
; point, which is what this probe pins.
;
; Build: nasm -f elf32 -o x87mf.o x87mf.asm
;        ld.lld -m elf_i386 -T ../pm/pm_smoke.ld -o x87mf.elf x87mf.o

bits 32

%define GDT      0x2000
%define GDTPTR   0x1f00
%define IDT      0x3000
%define IDTPTR   0x2f00
%define B        0x8000
%define ENV      0x8100
%define WAITPC   0x9000
%define MFHIT    0x9004

%macro SET_GATE 2
  mov eax, %2
  mov [IDT + %1 * 8], ax
  mov word [IDT + %1 * 8 + 2], 0x08
  mov byte [IDT + %1 * 8 + 4], 0
  mov byte [IDT + %1 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + %1 * 8 + 6], ax
%endmacro

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
  mov esp, 0x7000
  jmp 0x08:.reload_cs
.reload_cs:

  mov word [IDTPTR], 0x7ff
  mov dword [IDTPTR + 2], IDT
  xor eax, eax
  mov ecx, 0x400
  mov edi, IDT
  rep stosd
  SET_GATE 16, mf_handler
  SET_GATE 0, fail_handler
  SET_GATE 6, fail_handler
  SET_GATE 13, fail_handler
  SET_GATE 14, fail_handler
  lidt [IDTPTR]

  mov dword [MFHIT], 0
  mov eax, cr0
  or eax, 0x20              ; CR0.NE=1
  mov cr0, eax
  fninit
  mov word [B], 0x037e      ; IE unmasked
  fldcw [B]

  ; t1/t2: 0/0 raises IE but not #MF yet. FNSTSW AX is non-waiting.
  fldz
  fldz
  fdivp
  fnstsw ax
  test ax, 0x0001
  jz fail1
  test ax, 0x8000           ; B mirrors ES
  jz fail2

  ; t3: the next waiting instruction raises #MF and the saved EIP names it.
  mov dword [WAITPC], .wait_insn
.wait_insn:
  fld1
  cmp dword [MFHIT], 1
  jne fail3
  fnstsw ax
  test ax, 0x0001
  jnz fail4                 ; the handler's FNCLEX cleared it
  test ax, 0x8080
  jnz fail5

  ; t4: FNSTENV stores the old CW, then masks all exceptions.
  mov dword [MFHIT], 0
  mov word [B], 0x037e
  fldcw [B]
  fldz
  fldz
  fdivp
  fnstenv [ENV]
  cmp word [ENV], 0x037e
  jne fail6
  fnstcw [B]
  cmp word [B], 0x037f
  jne fail7
  fnstsw ax
  test ax, 0x8080           ; ES and B are clear
  jnz fail8
  test ax, 0x0001           ; IE stays set (masked)
  jz fail9
  fld1                      ; no #MF: exceptions are masked now
  cmp dword [MFHIT], 0
  jne fail10

pass:
  mov al, 5
  mov dx, 0xf4
  out dx, al
.hang_pass:
  hlt
  jmp .hang_pass

fail1:  mov al, 2
        jmp report
fail2:  mov al, 3
        jmp report
fail3:  mov al, 4
        jmp report
fail4:  mov al, 5
        jmp report
fail5:  mov al, 6
        jmp report
fail6:  mov al, 7
        jmp report
fail7:  mov al, 8
        jmp report
fail8:  mov al, 9
        jmp report
fail9:  mov al, 10
        jmp report
fail10: mov al, 11
        jmp report
fail11: mov al, 12
        jmp report

fail:
  mov al, 1
report:
  mov dx, 0xf4
  out dx, al
.hang_fail:
  hlt
  jmp .hang_fail

mf_handler:
  mov eax, [esp]            ; #MF pushes no error code
  cmp eax, [WAITPC]
  jne fail11
  mov dword [MFHIT], 1
  fnclex
  iret

fail_handler:
  jmp fail
