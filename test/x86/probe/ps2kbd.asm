; PS/2 keyboard on the 8042's keyboard port (not an acceptance test).
;
; Two halves, the same split as ps2mouse.asm:
;
;   * the command set, which the guest drives itself: 0xf0 (query / select the
;     scancode set), 0xf2 (read the ID), 0xee (echo), 0xed / 0xf3 (the LED and
;     typematic parameters, accepted and without effect here), 0xf4 / 0xf5
;     (scanning on / off), 0xff (reset), and an unknown command, which must draw
;     the resend rather than an ACK.
;   * the translation matrix, which no guest can drive: the *host* has to press
;     a key, so the key events come from CEMU_DEBUG=key= (the same synthetic
;     source the window's keyboard drives). With the controller's translation
;     bit set the guest sees the host's set-1 bytes; with it clear and the
;     keyboard in set 2 the guest must see the set-2 equivalents — including the
;     0xf0 break prefix and the 0xe0 extended prefix.
;
; The replies are the translated ones: a translating 8042 turns the keyboard's
; "I am in set 2" into 0x41 and the MF2 ID 0xab 0x83 into 0xab 0x41 (aeb
; scancodes-10 §10.3's table — 1, 2, 3 -> 43, 41, 3f, and 83 -> 41).
;
; Build: nasm -f elf32 -o ps2kbd.o ps2kbd.asm
;        ld.lld -m elf_i386 -T ps2kbd.ld -o ps2kbd.elf ps2kbd.o
; Run:   cemu --machine x86 --isa x86 ps2kbd.elf \
;            CEMU_DEBUG=key=0x1e@100000,key=0x1e:0:1@100000,key=0x48:1@100000,key=0x48:1:1@100000
;        qemu-system-i386 -kernel ps2kbd.elf -display none -no-reboot \
;            -serial file:ps2kbd_qemu.txt -monitor stdio
;        (the QEMU run needs the monitor: `sendkey a 10` then `sendkey up 10`,
;         each on a line of its own, once the guest is spinning)
; Exit: debug-exit 5 when everything checks out, 3 when the command half did but
; no host key arrived, 1 on a failure.

  bits 32

%define DBGEXIT 0xf4
%define BUDGET  20000000    ; spin budget for the key half (loop iterations)

; Read one reply byte from the controller and require %1 in it.
%macro RD 1
  call kbd_read
  mov [got], al
  mov byte [exp], %1
  cmp al, %1
  je %%ok
  jmp fail
%%ok:
%endmacro

; Require %1 in AL (for values already read).
%macro CHK 1
  mov [got], al
  mov byte [exp], %1
  cmp al, %1
  je %%ok
  jmp fail
%%ok:
%endmacro

section .multiboot
align 4
  dd 0x1BADB002                 ; magic
  dd 0x3                        ; PAGE_ALIGN | MEMORY_INFO
  dd -(0x1BADB002 + 0x3)        ; checksum

section .text
global _start

_start:
  cli
  call uart_init

  ; ---- the command set ------------------------------------------------------
  ; 0xf0 0x00 reads the scancode set back. The keyboard answers 2 (the AT's
  ; default) and the controller's translation turns that into 0x41.
  mov al, 0xf0
  call kbd_write
  RD 0xfa
  mov al, 0x00
  call kbd_write
  RD 0xfa
  RD 0x41
  ; 0xf0 0x01 selects set 1: the reply is the untranslated-looking 0x43, which
  ; is what a translating 8042 makes of the 1 the keyboard actually sent.
  mov al, 0xf0
  call kbd_write
  RD 0xfa
  mov al, 0x01
  call kbd_write
  RD 0xfa
  mov al, 0xf0
  call kbd_write
  RD 0xfa
  mov al, 0x00
  call kbd_write
  RD 0xfa
  RD 0x43
  ; Back to set 2, then a request for set 3. This keyboard claims sets 1 and 2
  ; only, so it draws the resend an unacceptable parameter draws; the answer is
  ; printed rather than asserted because QEMU accepts set 3 and switches to it.
  mov al, 0xf0
  call kbd_write
  RD 0xfa
  mov al, 0x02
  call kbd_write
  RD 0xfa
  mov al, 0xf0
  call kbd_write
  RD 0xfa
  mov al, 0x03
  call kbd_write
  call kbd_read
  mov [set3], al
  mov esi, m_set3
  call puts
  mov al, [set3]
  call puthex8
  ; Read the ID: ACK + the two bytes, the second translated.
  mov al, 0xf2
  call kbd_write
  RD 0xfa
  RD 0xab
  RD 0x41
  ; Echo answers itself.
  mov al, 0xee
  call kbd_write
  RD 0xee
  ; An unknown command draws the resend (aeb §12: "Each command (other than
  ; 0xfe) is ACKed by 0xfa. Each unknown command is NACKed by 0xfe").
  mov al, 0x1f
  call kbd_write
  RD 0xfe
  ; 0xed and 0xf3 take a parameter byte, and both bytes are ACKed; neither has
  ; an observable effect on a machine with no LEDs and a host-generated repeat.
  mov al, 0xed
  call kbd_write
  RD 0xfa
  mov al, 0x07
  call kbd_write
  RD 0xfa
  mov al, 0xf3
  call kbd_write
  RD 0xfa
  mov al, 0x00
  call kbd_write
  RD 0xfa
  ; Scanning off and on again.
  mov al, 0xf5
  call kbd_write
  RD 0xfa
  mov al, 0xf4
  call kbd_write
  RD 0xfa
  ; Reset: ACK then the self-test byte, and the set goes back to the default.
  mov al, 0xff
  call kbd_write
  RD 0xfa
  RD 0xaa
  mov al, 0xf0
  call kbd_write
  RD 0xfa
  mov al, 0x00
  call kbd_write
  RD 0xfa
  RD 0x41

  ; ---- the translation matrix -----------------------------------------------
  ; The controller's command byte carries the translation bit in bit 6. The
  ; guest reads it, and each phase below sets the bit and the keyboard's
  ; scancode set the way a driver would, then lets the injected host key
  ; arrive.
  mov al, 0x20
  out 0x64, al
  call kbd_read
  mov [cmdb], al
  mov esi, m_cmd
  call puts
  mov al, [cmdb]
  call puthex8

  ; Phase A: translation on, set 2 — the guest must see the host's own set-1
  ; bytes: 'a' 0x1e / 0x9e, Up 0xe0 0x48 / 0xe0 0xc8.
  call set_translate_on
  call drain
  mov byte [nbytes], 6
  call read_n
  call report_bytes

  ; Phase B: translation off, set 2 — now the set-2 encoding: 'a' 0x1c /
  ; 0xf0 0x1c, Up 0xe0 0x75 / 0xe0 0xf0 0x75.
  call set_translate_off
  call drain
  mov byte [nbytes], 8
  call read_n
  call report_bytes

  ; Phase C: translation off but the keyboard in set 1 — aeb §10.1: "Set 1
  ; should not be translated", and the host already speaks it, so the bytes go
  ; through unchanged even with the bit clear.
  mov al, 0xf0
  call kbd_write
  RD 0xfa
  mov al, 0x01
  call kbd_write
  RD 0xfa
  call drain
  mov byte [nbytes], 6
  call read_n
  call report_bytes

  call crlf
  mov esi, m_ok
  call puts
  mov al, 5
  jmp exit

fail:
  call crlf
  mov esi, m_fail
  call puts
  mov al, [exp]
  call puthex8
  mov esi, m_got
  call puts
  mov al, [got]
  call puthex8
  call crlf
  mov al, 1
exit:
  mov dx, DBGEXIT
  out dx, al
  hlt
  jmp $

; ---- the phases' helpers ----------------------------------------------------

; The command byte's bit 6 is the translation bit (0x45 has it set, 0x05
; clear); everything else is left as the machine came up with it.
set_translate_on:
  mov al, [cmdb]
  or al, 0x40
  jmp set_cmd_byte
set_translate_off:
  mov al, [cmdb]
  and al, 0xbf
set_cmd_byte:
  mov [cmdb], al
  mov bl, al
  mov al, 0x60
  out 0x64, al
  mov al, bl
  out 0x60, al
  ret

; Read [nbytes] bytes from the controller into pkt, with a spin budget per
; byte so a run with no injected keys reports instead of hanging. A byte that
; never arrives becomes 0xff, which no check expects.
read_n:
  movzx ecx, byte [nbytes]
  mov edi, pkt
.next:
  push ecx
  mov ecx, BUDGET
.spin:
  in al, 0x64
  test al, 1
  jnz .got
  dec ecx
  jnz .spin
  pop ecx
.fill:
  mov al, 0xff
  stosb
  loop .fill
  mov byte [nokey], 1
  ret
.got:
  mov dx, 0x60
  in al, dx
  stosb
  pop ecx
  dec ecx
  jnz .next
  ret

report_bytes:
  mov esi, m_seq
  call puts
  movzx ecx, byte [nbytes]
  mov esi, pkt
.print:
  lodsb
  call puthex8
  loop .print
  ret

; Send AL to the keyboard (port 0x60) and nothing else: the controller routes a
; data-port write to the keyboard unless 0xd4 came first.
kbd_write:
  out 0x60, al
  ret

; Wait for output-buffer-full and read 0x60. A timeout answers 0xff, which no
; check expects.
kbd_read:
  push ecx
  mov ecx, 0x400000
.wait:
  mov dx, 0x64
  in al, dx
  test al, 1
  jnz .got
  loop .wait
  mov al, 0xff
  pop ecx
  ret
.got:
  mov dx, 0x60
  in al, dx
  pop ecx
  ret

; Drop whatever the queue already holds, without waiting: each phase must judge
; the bytes its own host event produced.
drain:
  mov dx, 0x64
  in al, dx
  test al, 1
  jz .done
  mov dx, 0x60
  in al, dx
  jmp drain
.done:
  ret

puthex8:                        ; AL
  push eax
  shr al, 4
  call puthexdig
  pop eax
  call puthexdig
  ret

puthexdig:
  and al, 0x0f
  cmp al, 10
  jb .digit
  add al, 'a' - 10
  jmp putc
.digit:
  add al, '0'
  jmp putc

puts:
  lodsb
  test al, al
  jz .done
  call putc
  jmp puts
.done:
  ret

crlf:
  mov al, 13
  call putc
  mov al, 10
  jmp putc

putc:
  push eax
  mov dx, 0x3fd
.wait:
  in al, dx
  test al, 0x20
  jz .wait
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

cmdb:      db 0
set3:      db 0
exp:       db 0
got:       db 0
nokey:     db 0
nbytes:    db 0
pkt:       times 8 db 0
m_cmd:     db " cmd=", 0
m_set3:    db " s3=", 0
m_seq:     db " seq=", 0
m_ok:      db "ps2kbd ok", 13, 10, 0
m_fail:    db "ps2kbd FAIL exp=", 0
m_got:     db " got=", 0
