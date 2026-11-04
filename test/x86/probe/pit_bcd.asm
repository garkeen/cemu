; Intel 8254 BCD counting (not an acceptance test -- a probe that pins the
; control word's BCD bit, which used to be stored but never modelled).
;
; Two checks on channel 2 (no IRQ, so the guest sees no timer interrupt):
;
;   hi   load 0x1234 in BCD mode and latch immediately: the high byte must be
;        0x12. A binary countdown of 1234 decimal would read 0x04 (0x04d2),
;        which is what the unmodelled path answered.
;   tens re-program with the 8-bit access and BCD 0x0016, then sample the low
;        byte 200000 times. Every sample must be valid BCD (each nibble <= 9),
;        and at least one sample must be 0x11..0x16: a binary counter would
;        step through 0x0a..0x0f, which are not BCD digits.
;
; Reference: Intel 8254 datasheet, control word bit 0 (BCD) and "Binary/BCD
; counting"; the counter decrements the decimal value, so 0x9999 is followed by
; 0x0000. QEMU is not a reference here: hw/timer/i8254.c stores s->bcd and its
; status-latch comment says "XXX: add BCD", but the counter stays binary, so
; QEMU fails this probe. No dual run.
;
; Build: nasm -f elf32 -o pit_bcd.o pit_bcd.asm
;        ld.lld -m elf_i386 -T vbe_mode.ld -o pit_bcd.elf pit_bcd.o
; Run:   cemu --machine x86 --isa x86 pit_bcd.elf
; Exit: debug-exit 5 (status 11) when both checks pass, 1 on the first failure.

  bits 32

%define PIT_CH2  0x42
%define PIT_CTL  0x43
%define DBGEXIT  0xf4

%macro P 1
  mov esi, %1
  call puts
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
  call uart_init

  ; Channel 2, LSB/MSB, mode 2 (rate generator), BCD. The reload is 0x1234.
  mov al, 0xb5
  out PIT_CTL, al
  mov al, 0x34
  out PIT_CH2, al
  mov al, 0x12
  out PIT_CH2, al

  ; Latch the current count (channel 2), then read the two bytes. The BCD
  ; readback of 1234 is 0x12 0x34 (a binary counter gives 0x04 0xd2).
  mov al, 0x80
  out PIT_CTL, al
  mov dx, PIT_CH2
  in al, dx
  mov [lo1], al
  in al, dx
  mov [hi1], al
  cmp al, 0x12
  jne .fail_hi
  mov al, [lo1]
  call check_bcd
  jc .fail_lo

  ; 8-bit access, BCD 0x0016. Sample the low byte many times: 16..10 are
  ; 0x16..0x10 in BCD, while a binary counter goes 0x10,0x0f..0x01 and would
  ; fail the digit check at 0x0f.
  mov al, 0x95
  out PIT_CTL, al
  mov al, 0x16
  out PIT_CH2, al
  mov dx, PIT_CH2
  mov ecx, 200000
  xor bl, bl                    ; 1 = a sample in 0x11..0x16 was seen
.loop:
  in al, dx
  mov ah, al
  call check_bcd_ah
  jc .fail_sample
  cmp ah, 0x11
  jb .next
  cmp ah, 0x16
  ja .next
  mov bl, 1
.next:
  loop .loop
  test bl, bl
  jz .fail_tens

  P m_ok
  P m_crlf
  mov al, 5
  jmp exit

.fail_hi:
  mov byte [where], 1
  mov [got], al
  jmp fail
.fail_lo:
  mov byte [where], 2
  jmp fail
.fail_sample:
  mov byte [where], 3
  mov [got], ah
  jmp fail
.fail_tens:
  mov byte [where], 4
  mov byte [got], 0
  jmp fail

; CF = 1 when AL is not two BCD digits (each nibble <= 9).
check_bcd:
  mov ah, al
check_bcd_ah:
  mov al, ah
  and al, 0x0f
  cmp al, 9
  ja .bad
  mov al, ah
  shr al, 4
  cmp al, 9
  ja .bad
  clc
  ret
.bad:
  stc
  ret

fail:
  P m_fail
  mov al, [where]
  call hex8
  P m_got
  mov al, [got]
  call hex8
  call crlf
  mov al, 1
  jmp exit

exit:
  mov dx, DBGEXIT
  out dx, al
  jmp $

; === serial output ==========================================================
hex8:                           ; AL as two hex digits
  push eax
  mov ah, al
  shr al, 4
  call hexdigit
  mov al, ah
  call hexdigit
  pop eax
  ret

hexdigit:                       ; AL = 0..15
  and al, 0x0f
  cmp al, 10
  jb .digit
  add al, 'a' - 10
  jmp putc
.digit:
  add al, '0'
  jmp putc

puts:                           ; ESI = string, until NUL
  lodsb
  test al, al
  jz .done
  call putc
  jmp puts
.done:
  ret

putc:                           ; AL = character, waits for THRE
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

section .data
lo1:    db 0
hi1:    db 0
where:  db 0
got:    db 0
m_ok:   db "pit-bcd ok", 0
m_fail: db "pit-bcd FAIL at=", 0
m_got:  db " got=", 0
m_crlf: db 13, 10, 0
