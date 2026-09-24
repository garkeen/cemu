; Microsoft serial mouse on COM1 (not an acceptance test).
;
; The device a 1985-era guest drives: it has no registers and no bus presence, it
; just sends 3-byte packets out of the serial port as if they had arrived on the
; SIN pin. So the probe is a receiver: it configures COM1 the way such a driver
; does (1200 baud, 7 data bits, no parity, one stop bit, receiver FIFO on — the
; FIFO is load-bearing: a 16550 with FCR.FE = 0 keeps one byte and drops the
; rest of the burst, so a three-byte packet could never be assembled) and reads
; the packets.
;
; The host events come from CEMU_DEBUG=mouse= (-mouse serial feeds the serial
; mouse instead of the PS/2 one): a button press, then a movement with the
; button held, so two packets arrive —
;
;   60 00 00   the button's: bit 6 always set, bit 5 left, no movement
;   6c 0a 36   the movement's: dx = +10 (10), dy = -10 (0x36 = -10 in the six
;              low bits, with 11 in byte 1's high pair)
;
; Build: nasm -f elf32 -o sermouse.o sermouse.asm
;        ld.lld -m elf_i386 -T sermouse.ld -o sermouse.elf sermouse.o
; Run:   cemu --machine x86 --isa x86 -mouse serial sermouse.elf
;            (CEMU_DEBUG=mouse=0:0:1@100000,mouse=10:-10:1@100000)
;        qemu-system-i386 -kernel sermouse.elf -serial msmouse -display none \
;            -no-reboot -device isa-debug-exit,iobase=0xf4,iosize=0x4 -monitor stdio
;        (the monitor: `mouse_button 1` then `mouse_move 10 10`)
; Exit: debug-exit 5 when both packets check out, 3 when no host event came,
; 1 on a mismatch.
;
; There is no QEMU dual run for this probe, and that is a QEMU-side fact rather
; than a disagreement about the protocol (measured 2026-09-24): the monitor's
; mouse commands go through QEMU's input layer, which hands an event to the
; *first* registered mouse handler — on a default PC that is the machine's PS/2
; mouse, so the msmouse chardev never sees it. `-trace "serial_*"` shows the
; guest polling 0x3FD forever with LSR = 0x60 (THRE|TEMT, DR clear) and no
; msmouse trace point firing at all; `-M isapc` (no PS/2 mouse) behaves the
; same. The packet layout this probe asserts therefore rests on QEMU's
; chardev/msmouse.c field construction, not on a double run.

  bits 32

%define DBGEXIT 0xf4
%define BUDGET  20000000    ; spin budget per byte (loop iterations)

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

  ; Wait for the two packets, then report them.
  mov byte [nbytes], 6
  call read_n
  mov esi, m_pkt
  call puts
  movzx ecx, byte [nbytes]
  mov esi, pkt
.print:
  lodsb
  call puthex8
  loop .print
  call crlf
  cmp byte [nokey], 0
  je .check
  mov esi, m_nohost
  call puts
  mov al, 3
  jmp exit

.check:
  mov al, [pkt + 0]
  CHK 0x60
  mov al, [pkt + 1]
  CHK 0x00
  mov al, [pkt + 2]
  CHK 0x00
  mov al, [pkt + 3]
  CHK 0x6c
  mov al, [pkt + 4]
  CHK 0x0a
  mov al, [pkt + 5]
  CHK 0x36
  mov esi, m_ok
  call puts
  mov al, 5
  jmp exit

fail:
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

; Read [nbytes] bytes from COM1's receiver into pkt. The line-status bit 0 is
; "data ready"; a byte that never arrives becomes 0xff, which no check expects.
read_n:
  movzx ecx, byte [nbytes]
  mov edi, pkt
.next:
  push ecx
  mov ecx, BUDGET
.spin:
  mov dx, 0x3fd
  in al, dx
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
  mov dx, 0x3f8
  in al, dx
  stosb
  pop ecx
  dec ecx
  jnz .next
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

; COM1 the way a serial-mouse driver sets it up: 1200 baud (divisor 96), 7 data
; bits, no parity, one stop bit, receiver FIFO on. The format is not what makes
; the probe work (the emulated receiver hands the bytes over whatever it says),
; but the FIFO is: a 16550 with FCR.FE = 0 holds exactly one byte and drops the
; rest of the burst with an overrun, and a mouse packet is three bytes sent
; back to back, so a receiver that never enables the FIFO can never assemble
; one. QEMU's serial_receive1 is the arbiter here — cemu's receiver buffers
; regardless of FCR.FE (AGENTS.md D32), so only a FIFO-enabled probe runs the
; same way on both.
uart_init:
  mov dx, 0x3fb
  mov al, 0x82                 ; DLAB set, 7 bits, no parity, 1 stop
  out dx, al
  mov dx, 0x3f8
  mov al, 96                   ; 115200 / 1200
  out dx, al
  mov dx, 0x3f9
  xor al, al
  out dx, al
  mov dx, 0x3fb
  mov al, 0x02                 ; DLAB clear, 7 bits, no parity, 1 stop
  out dx, al
  mov dx, 0x3fa
  mov al, 0xc7                 ; FCR: FIFO on, both queues cleared, level 14
  out dx, al
  ret

exp:       db 0
got:       db 0
nokey:     db 0
nbytes:    db 0
pkt:       times 6 db 0
m_pkt:     db "pkt=", 0
m_ok:      db "sermouse ok", 13, 10, 0
m_nohost:  db "sermouse: no host event", 13, 10, 0
m_fail:    db "sermouse FAIL exp=", 0
m_got:     db " got=", 0
