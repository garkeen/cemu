; PS/2 mouse on the 8042's auxiliary port (not an acceptance test).
;
; The mouse is a byte-stream device behind the controller: commands go down the
; wire with 0xd4 ("the next 0x60 write is the auxiliary device's"), the device
; answers ACK/reply/data bytes into the controller's AUX output queue, status
; bit 5 marks a byte as the mouse's, and the queue raises IRQ12.
;
; Two halves, because only one of them is reachable from the guest:
;
;   * the protocol — 0xa8 opens the auxiliary port, the command set answers
;     (reset/BAT/id, the wheel handshake, status, resolution, sample rate), the
;     0xeb poll returns a packet, an unknown command draws a resend, 0xa7 shuts
;     the interface down. The guest drives all of that itself.
;   * the host event path — a probe running as a guest cannot move a pointer,
;     so the movement comes from CEMU_DEBUG=mouse= (a synthetic event into the
;     board's pointer sink, the same sink the display window drives). Without
;     it the probe reports no-host instead of failing.
;
; IRQ12 is observed through the 8259 slave's interrupt request register rather
; than through an interrupt handler: the IRR latches a request whatever the mask
; says, so OCW3 (0x0a) + a read of 0xa1 shows the line with no IDT, no remap
; and no sti — and identically on QEMU.
;
; A multiboot ELF rather than a 512-byte boot sector: the checks do not fit in
; one sector (a sector is also all QEMU's BIOS loads from a -drive image), and
; the flat protected-mode entry is the contract the pm suite already runs under
; (cemu's multiboot detection and QEMU's -kernel agree on it).
;
; Build: nasm -f elf32 -o ps2mouse.o ps2mouse.asm
;        ld.lld -m elf_i386 -T ps2mouse.ld -o ps2mouse.elf ps2mouse.o
; Run:   cemu --machine x86 --isa x86 ps2mouse.elf    (CEMU_DEBUG=mouse=10:-10:1:1@100000)
;        qemu-system-i386 -kernel ps2mouse.elf -display none -no-reboot \
;            -serial file:ps2mouse_qemu.txt -monitor stdio
;        (the QEMU run needs the monitor to send `mouse_move 10 -10` and
;         `mouse_button 1` once the guest is spinning)
; Exit: debug-exit value 5 when the protocol and the host packet both check
; out, 3 when the protocol did but no host event came, 1 on a failure.

  bits 32

%define DBGEXIT 0xf4
%define BUDGET  20000000    ; spin budget for the host half (loop iterations).
                            ; On cemu the event lands within the injection
                            ; period; on QEMU this is how long the guest waits
                            ; for the monitor's mouse_move.

; Read one reply byte from the device and require %1 in it.
%macro RD 1
  call mouse_read
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

  ; Open the auxiliary port and route its interrupt the way a driver does:
  ; 0xa8, read the command byte (0x20), set the AUX and keyboard interrupt bits
  ; (1 and 0), write it back (0x60). The byte is printed rather than asserted —
  ; its power-on value is a controller property, not a mouse one.
  mov al, 0xa8
  out 0x64, al
  mov al, 0x20
  out 0x64, al
  call mouse_read
  mov [cmdb], al
  mov esi, m_cmd
  call puts
  mov al, [cmdb]
  call puthex8
  mov al, [cmdb]
  or al, 0x03
  mov bl, al
  mov al, 0x60
  out 0x64, al
  mov al, bl
  out 0x60, al

  ; Reset: ACK, the power-on-reset byte, then the device id.
  mov al, 0xff
  call mouse_byte
  RD 0xfa
  RD 0xaa
  RD 0x00
  ; Identify: ACK + the plain mouse's id.
  mov al, 0xf2
  call mouse_byte
  RD 0xfa
  RD 0x00

  ; The wheel handshake: 200, 100, 80 Hz in a row makes the 0xf2 that follows
  ; report the wheel mouse (0x03), which is what makes a packet four bytes.
  mov esi, rates
  mov ecx, 3
.rates:
  mov al, 0xf3
  call mouse_byte
  RD 0xfa
  lodsb
  call mouse_byte              ; the parameter byte needs its own 0xd4
  RD 0xfa
  loop .rates
  mov al, 0xf2
  call mouse_byte
  RD 0xfa
  RD 0x03

  ; Status: ACK + status byte + resolution code + sample rate. The three values
  ; are what a driver reads to learn the device's state, so they are printed as
  ; well as asserted: nothing has changed the defaults (100 Hz, 4 counts/mm)
  ; except the 80 Hz the handshake left behind.
  mov al, 0xe9
  call mouse_byte
  RD 0xfa
  mov esi, m_st
  call puts
  call mouse_read
  mov [st + 0], al
  CHK 0x00                     ; status: stream mode, disabled, 1:1, no buttons
  call mouse_read
  mov [st + 1], al
  CHK 0x02                     ; resolution: 4 counts/mm
  call mouse_read
  mov [st + 2], al
  CHK 0x50                     ; sample rate: the handshake's 80 Hz
  mov al, [st + 0]
  call puthex8
  mov al, [st + 1]
  call puthex8
  mov al, [st + 2]
  call puthex8

  ; Poll: ACK + one packet now. With no movement reported the packet is the
  ; idle one — the always-1 bit, no signs, no buttons, zero counts — which pins
  ; the byte order and the first byte's layout, and the fourth byte proves the
  ; wheel mouse the handshake announced.
  mov al, 0xeb
  call mouse_byte
  RD 0xfa
  RD 0x08
  RD 0x00
  RD 0x00
  RD 0x00

  ; An unknown command draws the resend — printed rather than asserted,
  ; because this QEMU build (and v86) answer nothing at all to one, while
  ; QEMU's source has the resend on its default branch. The value is what
  ; run.sh checks.
  mov al, 0xa5
  call mouse_byte
  call mouse_read
  mov [unk], al
  mov esi, m_unk
  call puts
  mov al, [unk]
  call puthex8

  ; 0xa7 shuts the auxiliary interface down: the controller holds the device's
  ; clock line low, so its answer never reaches the output queue and
  ; output-buffer-full stays clear. The bit is printed, not asserted — QEMU
  ; keeps queueing the answer and only v86 and the hardware agree with this
  ; model. 0xa8 brings the interface back.
  mov al, 0xa7
  out 0x64, al
  mov al, 0xd4
  out 0x64, al
  mov al, 0xf2
  out 0x60, al
  in al, 0x64
  and al, 1
  mov [auxoff], al
  mov al, 0xa8
  out 0x64, al
  mov al, 0xf2
  call mouse_byte
  RD 0xfa
  RD 0x03                     ; the id the wheel handshake left behind

  ; Drop anything the queue still holds, without waiting for more. The check
  ; above is exactly where the two models part company: this one stops the
  ; bytes at the wire while the interface is off (what 0xa7 is for), QEMU keeps
  ; queueing them, so QEMU has an extra (fa, id) pair waiting here. Draining
  ; keeps the packet reads below aligned in both, and `auxoff` in the report is
  ; what records the divergence.
  call drain

  ; ---- the host event path --------------------------------------------------
  ; Stream mode + reporting on, then wait for the injected host events. The
  ; host presses the left button and then moves +10/-10 with one wheel detent —
  ; QEMU's monitor sends that as `mouse_button 1` then `mouse_move 10 -10 1`,
  ; and CEMU_DEBUG=mouse= as two items — so two packets arrive: the button's
  ; (no movement) and the movement's (the button still held, so its first byte
  ; carries both). The second is the one that pins the layout: bit 5 the Y
  ; sign, bit 0 the left button, the counts in 9-bit two's complement, and the
  ; wheel byte the handshake's device id made four bytes wide.
  mov al, 0xea
  call mouse_byte
  RD 0xfa
  mov al, 0xf4
  call mouse_byte
  RD 0xfa
  mov ecx, BUDGET
.spin:
  in al, 0x64
  and al, 0x21                 ; output buffer full + the byte is the mouse's
  cmp al, 0x21
  je .host
  loop .spin
  mov esi, m_nohost
  call puts
  call crlf
  mov al, 3
  jmp exit
.host:
  ; IRQ12 latched: the slave's request register bit 4 (IRQ12 = its line 4). The
  ; IRR/ISR pair is selected by OCW3 on the command port and read from that same
  ; port (A0 = 0), not from the data port the mask lives in.
  mov al, 0x0a
  out 0xa0, al                 ; OCW3: read the request register
  in al, 0xa0
  mov [irr], al
  ; Both packets, out of the AUX output queue.
  mov edi, pkt
  mov ecx, 8
.read:
  call mouse_read
  stosb
  loop .read

  ; ---- report ---------------------------------------------------------------
  mov esi, m_aux
  call puts
  mov al, [auxoff]
  call puthex8
  mov esi, m_irr
  call puts
  mov al, [irr]
  call puthex8
  mov esi, m_pkt
  call puts
  mov ecx, 8
  mov edi, pkt
.print:
  mov al, [edi]
  inc edi
  call puthex8
  loop .print
  call crlf
  ; The button's packet, then the movement's.
  mov al, [pkt + 0]
  CHK 0x09
  mov al, [pkt + 1]
  CHK 0x00
  mov al, [pkt + 2]
  CHK 0x00
  mov al, [pkt + 3]
  CHK 0x00
  mov al, [pkt + 4]
  CHK 0x29
  mov al, [pkt + 5]
  CHK 0x0a
  mov al, [pkt + 6]
  CHK 0xf6
  mov al, [pkt + 7]
  CHK 0x01
  mov al, [irr]
  and al, 0x10                 ; IRQ12 must have been requested
  CHK 0x10
  ; `auxoff` is the third divergence and stays unasserted for the same reason
  ; (QEMU answers the probe's 0xf2 while the interface is off); run.sh checks
  ; the value in the report line instead.
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

; Discard whatever the controller's output queue already holds, without waiting
; for more.
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

; Send AL down the auxiliary wire: 0xd4 marks the next 0x60 write as the
; device's, and every byte needs its own (SeaBIOS's ps2_sendbyte, QEMU
; pckbd.c's KBD_CCMD_WRITE_MOUSE).
mouse_byte:
  mov bl, al
  mov al, 0xd4
  out 0x64, al
  mov al, bl
  out 0x60, al
  ret

; Wait for output-buffer-full and read 0x60. A timeout answers 0xff, which no
; check expects, so a missing reply fails with a transcript instead of hanging.
mouse_read:
  push ecx
  mov ecx, 0x10000             ; a 32-bit `loop` counts ecx, so the count is
                               ; explicit: cx = 0 would be 65536 iterations in
                               ; real mode and 4 billion here
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

rates:     db 200, 100, 80
cmdb:      db 0
st:        times 3 db 0
auxoff:    db 0
unk:       db 0
irr:       db 0
exp:       db 0
got:       db 0
pkt:       times 8 db 0
m_cmd:     db "cmd=", 0
m_st:      db " st=", 0
m_aux:     db " auxoff=", 0
m_unk:     db " unk=", 0
m_irr:     db " irr=", 0
m_pkt:     db " pkt=", 0
m_ok:      db "ps2mouse ok", 13, 10, 0
m_nohost:  db "ps2mouse: protocol ok, no host event", 13, 10, 0
m_fail:    db "ps2mouse FAIL exp=", 0
m_got:     db " got=", 0
