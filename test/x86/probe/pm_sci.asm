; PIIX4 PM1a fixed events, SCI and S5 (not an acceptance test -- a probe that
; pins one chipset function).
;
; PMBASE is programmed through 00:01.3's configuration space, the block is
; enabled with PMREGMISC bit 0, and the probe then:
;   1. reads the reset state of PM1_STS/PM1_EN/PM1_CNT;
;   2. writes GBL_RLS and finds GBL_STS set, then clears it write-one-to-clear;
;   3. enables GBL_STS as an SCI with SCI_EN and sees IRQ9 asserted (the 8259
;      pair is remapped and IRQ9 is selected level-sensitive in the ELCR);
;   4. clears the event and sees IRQ9 drop;
;   5. writes SLP_TYP=S5 | SLP_EN, which powers the machine off (the session
;      exits with status 0).
;
; Reference: ACPI 4.8 (PM1_STS/PM1_EN/PM1_CNT bits), PIIX4 datasheet (PMBASE
; 0x40, PMREGMISC 0x80), QEMU hw/acpi/piix4.c (SCI on IRQ9, S5 -> shutdown).
;
; Build: nasm -f elf32 -o pm_sci.o pm_sci.asm
;        ld.lld -m elf_i386 -T vbe_mode.ld -o pm_sci.elf pm_sci.o
; Run:   cemu --machine x86 --isa x86 pm_sci.elf
;        qemu-system-i386 -machine pc -m 32 -kernel pm_sci.elf -vga std \
;            -display none -no-reboot -serial file:pm_sci_qemu.txt
; Exit: the guest powers off (status 0); "pm-sci ok" is the verdict.

  bits 32

%define CFG_ADDR 0xcf8
%define CFG_DATA 0xcfc
%define PMBASE   0x600

section .multiboot
align 4
  dd 0x1BADB002
  dd 0x3
  dd -(0x1BADB002 + 0x3)

section .text
global _start

_start:
  cli
  call uart_init

  ; PMBASE = 0x600 | I/O indicator, then PMREGMISC bit 0.
  mov eax, 0x80000b40        ; bus 0, dev 1, func 3, offset 0x40
  mov dx, CFG_ADDR
  out dx, eax
  mov dx, CFG_DATA
  mov eax, PMBASE | 1
  out dx, eax
  mov eax, 0x80000b80        ; PMREGMISC
  mov dx, CFG_ADDR
  out dx, eax
  mov dx, CFG_DATA
  mov al, 1
  out dx, al

  ; 1) reset state: no event, no enable, no sleep control
  mov byte [step_no], 1
  mov dx, PMBASE + 0
  mov ax, 0xffff            ; clear any power-on fixed event (QEMU's PM timer
  out dx, ax                ; overflow bit is already set when the probe runs)
  mov dx, PMBASE + 0
  in ax, dx
  test ax, ax
  jnz fail
  mov dx, PMBASE + 2
  in ax, dx
  test ax, ax
  jnz fail
  mov dx, PMBASE + 4
  in ax, dx
  test ax, ax
  jnz fail

  ; 2) GBL_RLS sets GBL_STS; the control bit does not stick and W1C clears.
  mov byte [step_no], 2
  mov dx, PMBASE + 4
  mov ax, 0x0004
  out dx, ax
  mov dx, PMBASE + 0
  in ax, dx
  test ax, 0x0020
  jz fail
  mov dx, PMBASE + 4
  in ax, dx
  test ax, 0x0004
  jnz fail
  mov dx, PMBASE + 0
  mov ax, 0x0020
  out dx, ax
  mov dx, PMBASE + 0
  in ax, dx
  test ax, 0x0020
  jnz fail

  ; 3) IRQ9 is the SCI. Remap the 8259 pair, open the cascade and IRQ9, make
  ;    IRQ9 level-sensitive, then enable GBL_STS as an SCI.
  mov byte [step_no], 3
  mov al, 0x11
  out 0x20, al
  out 0xa0, al
  mov al, 0x20
  out 0x21, al
  mov al, 0x28
  out 0xa1, al
  mov al, 0x04
  out 0x21, al
  mov al, 0x02
  out 0xa1, al
  mov al, 0x01
  out 0x21, al
  out 0xa1, al
  mov al, 0xfb              ; master: IRQ2 (cascade) open
  out 0x21, al
  mov al, 0xfd              ; slave: IRQ1 (IRQ9) open
  out 0xa1, al
  mov dx, 0x4d1
  mov al, 0x02              ; IRQ9 level-sensitive
  out dx, al

  mov dx, PMBASE + 2
  mov ax, 0x0020            ; enable GBL_STS
  out dx, ax
  mov dx, PMBASE + 4
  mov ax, 0x0001            ; SCI_EN
  out dx, ax
  mov dx, PMBASE + 4
  mov ax, 0x0005            ; SCI_EN | GBL_RLS -> event + SCI
  out dx, ax

  mov ecx, 0x100000
.wait_irr:
  mov dx, 0xa0
  in al, dx
  test al, 0x02             ; slave IRR bit 1 = IRQ9
  jnz .irr_set
  dec ecx
  jnz .wait_irr
  jmp fail
.irr_set:

  ; 4) W1C the event: a level-triggered line should drop the request too.
  mov dx, PMBASE + 0
  mov ax, 0x0020
  out dx, ax
  mov ecx, 0x100000
.wait_low:
  mov dx, 0xa0
  in al, dx
  test al, 0x02
  jz .irr_low
  dec ecx
  jnz .wait_low
  jmp fail
.irr_low:

  ; 4) S5: SLP_TYP = 5 (bits 12:10), SLP_EN = bit 13.
  mov byte [step_no], 4
  mov esi, m_pass
  call puts
  mov dx, PMBASE + 4
  mov ax, 0x3400
  out dx, ax
  ; The machine powers off; if it does not, the probe fails.
  jmp fail

fail:
  mov esi, m_fail
  call puts
  mov al, [step_no]
  add al, '0'
  call putc
  jmp $

puts:                         ; ESI = string, until NUL
  lodsb
  test al, al
  jz .done
  call putc
  jmp puts
.done:
  ret

putc:                         ; AL = character, waits for THRE
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

section .data
step_no:  db 0
m_pass:   db "pm-sci ok", 13, 10, 0
m_fail:   db "pm-sci FAIL step=", 0
