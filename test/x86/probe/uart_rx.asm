; 16550 receiver: one-byte depth with the FIFO off, 16 with it on, and the
; overrun bit both ways (not an acceptance test -- a probe that pins one
; device's receive semantics).
;
; The probe drives the chip through its own loopback (MCR bit 4), so no host
; input is involved and the sequence is deterministic: every THR write comes
; back on SIN. Waits for THRE are what make that true on a model whose
; transmitter is paced by character time (QEMU pops one byte per character;
; this machine's transmitter is idle at once, so the wait is a no-op there).
;
;   hold   LSR after one byte with the FIFO off: DR (0x61 = DR|THRE|TEMT)
;   over   LSR after a second byte arrives while the first is unread: OE too
;   keep   RBR then: the newest byte survives (QEMU keeps buf[0] in RBR and
;          latches OE for the byte it displaced)
;   clear  LSR after that read: DR and OE both gone
;   again  RBR after DR cleared: the last byte again (the non-FIFO branch of
;          QEMU's serial_ioport_read returns s->rbr)
;   fifo   LSR after four bytes with the FIFO on: DR, no OE
;   empty  RBR with the FIFO on and empty: 0
;   bytes  the four, in order
;   full   the overrun bit seen after a seventeenth byte into a full FIFO. It
;          is the OR of 64 LSR samples: reading LSR is what clears a latched
;          error bit, so a single read cannot both poll and report
;   head   RBR: the first byte of the block, so the queue did not drop it
;   tail   RBR after draining 15 more: the last one that fit
;   off    LSR after clearing FCR.FE with three bytes queued: the queue is
;          gone (turning the FIFO on or off empties it)
;
; Reference: 16550D datasheet (FCR bit 0 and the receiver's one-byte mode, LSR
; DR/OE, the FIFO's 16 bytes); QEMU hw/char/serial.c serial_receive1 (the
; `if (s->lsr & UART_LSR_DR) s->lsr |= UART_LSR_OE; s->rbr = buf[0];` branch for
; the FIFO-off case), serial_ioport_write's FCR handler (a change of FCR.FE
; folds in RFR|XFR) and serial_can_receive.
;
; Build: nasm -f elf32 -o uart_rx.o uart_rx.asm
;        ld.lld -m elf_i386 -T vbe_mode.ld -o uart_rx.elf uart_rx.o
; Run:   cemu --machine x86 --isa x86 uart_rx.elf
;        qemu-system-i386 -machine pc -m 32 -kernel uart_rx.elf -display none \
;            -no-reboot -device isa-debug-exit,iobase=0xf4,iosize=0x4 \
;            -serial file:uart_rx_qemu.txt
; Exit: debug-exit 5 (status 11) once the transcript is out.

  bits 32

%define COM 0x3f8
%define IER COM + 1
%define FCR COM + 2
%define LCR COM + 3
%define MCR COM + 4
%define LSR COM + 5
%define DBGEXIT 0xf4

%macro TX 1
  mov dx, COM
  mov al, %1
  out dx, al
%endmacro

%macro SETFCR 1
  mov dx, FCR
  mov al, %1
  out dx, al
%endmacro

; Wait until the transmitter has nothing left to send: on a model that paces
; the transmitter by character time this is what makes the byte reach the
; receiver before the next one is written (QEMU's xmit fifo is 16 deep and a
; burst past that would overwrite).
%macro WAITTHRE 0
  mov dx, LSR
%%wait:
  in al, dx
  test al, 0x20
  jz %%wait
%endmacro

%macro GETLSR 0
  mov dx, LSR
  in al, dx
%endmacro

%macro GETRBR 0
  mov dx, COM
  in al, dx
%endmacro

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

  ; 115200 8N1 (divisor 1), then the FIFO off and loopback on. Nothing is
  ; printed while loopback is on: a THR write would come back as input.
  mov dx, LCR
  mov al, 0x83
  out dx, al
  mov dx, COM
  mov al, 0x01
  out dx, al
  mov dx, IER
  xor al, al
  out dx, al
  mov dx, LCR
  mov al, 0x03
  out dx, al
  SETFCR 0x00
  mov dx, MCR
  mov al, 0x10
  out dx, al

  ; One byte with the FIFO off: it waits in RBR.
  TX 0x41
  GETLSR
  mov [hold], al

  ; A second byte while the first is still unread: the receiver is one deep, so
  ; the byte that was waiting is lost (OE) and the new one takes its place.
  TX 0x42
  GETLSR
  mov [over], al
  GETRBR
  mov [keep], al
  GETLSR
  mov [clear], al
  ; RBR keeps the byte after DR clears (QEMU's non-FIFO branch); an empty
  ; FIFO reads 0 instead.
  GETRBR
  mov [again], al

  ; FIFO on: four bytes fit, the receiver asks for service once and keeps order.
  SETFCR 0x07
  GETRBR
  mov [empty], al
  TX 0x43
  TX 0x44
  TX 0x45
  TX 0x46
  WAITTHRE
  GETLSR
  mov [fifo], al
  GETRBR
  mov [b0], al
  GETRBR
  mov [b1], al
  GETRBR
  mov [b2], al
  GETRBR
  mov [b3], al

  ; Sixteen bytes fill it; the seventeenth has nowhere to go. Reading LSR is
  ; what clears a latched error bit, so the poll keeps the OR of every sample
  ; it sees instead of trusting the last one.
  SETFCR 0x07
  mov ecx, 16
  mov bl, 0x30
.fill:
  mov dx, COM
  mov al, bl
  out dx, al
  WAITTHRE
  inc bl
  loop .fill
  TX 0x40
  xor bl, bl
  mov ecx, 64
.poll:
  GETLSR
  or bl, al
  loop .poll
  mov [full], bl
  GETRBR
  mov [head], al
  mov ecx, 14
.drain:
  GETRBR
  loop .drain
  GETRBR
  mov [tail], al

  ; Turning the FIFO off empties it: three queued bytes go away.
  SETFCR 0x07
  TX 0x58
  TX 0x59
  TX 0x5a
  WAITTHRE
  SETFCR 0x00
  GETLSR
  mov [off], al

  ; Loopback off, so the transcript goes out of the pin to the host.
  mov dx, MCR
  xor al, al
  out dx, al

  P m_head
  mov al, [hold]
  call hex8
  P m_over
  mov al, [over]
  call hex8
  P m_keep
  mov al, [keep]
  call hex8
  P m_clear
  mov al, [clear]
  call hex8
  P m_again
  mov al, [again]
  call hex8
  P m_fifo
  mov al, [fifo]
  call hex8
  P m_empty
  mov al, [empty]
  call hex8
  P m_bytes
  mov al, [b0]
  call hex8
  mov al, [b1]
  call hex8
  mov al, [b2]
  call hex8
  mov al, [b3]
  call hex8
  P m_full
  mov al, [full]
  call hex8
  P m_headb
  mov al, [head]
  call hex8
  P m_tail
  mov al, [tail]
  call hex8
  P m_off
  mov al, [off]
  call hex8
  P m_ok
  P m_crlf

  mov dx, DBGEXIT
  mov al, 5
  out dx, al
  jmp $

; AL as two hex digits.
hex8:
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
  mov dx, LSR
.wait:
  in al, dx
  test al, 0x20
  jz .wait
  pop edx
  pop eax
  mov dx, COM
  out dx, al
  ret

section .data
hold:   db 0
over:   db 0
keep:   db 0
clear:  db 0
again:  db 0
fifo:   db 0
empty:  db 0
b0:     db 0
b1:     db 0
b2:     db 0
b3:     db 0
full:   db 0
head:   db 0
tail:   db 0
off:    db 0
m_head:  db "uart hold=", 0
m_over:  db " over=", 0
m_keep:  db " keep=", 0
m_clear: db " clear=", 0
m_again: db " again=", 0
m_fifo:  db " fifo=", 0
m_empty: db " empty=", 0
m_bytes: db " bytes=", 0
m_full:  db " full=", 0
m_headb: db " head=", 0
m_tail:  db " tail=", 0
m_off:   db " off=", 0
m_ok:    db " ok", 0
m_crlf:  db 13, 10, 0
