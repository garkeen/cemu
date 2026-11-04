; Host input hold on COM1, driven through the serial mouse (not an acceptance
; test). Eight host mouse events are injected at the same instruction, so the
; device is asked to take 24 bytes at once while its receiver is one byte deep
; (FCR.FE=0). The host hold keeps them and Uart16550Poll/Uart16550Read feed them
; in as the guest drains; without it the one-deep receiver would keep only the
; last byte and 23 would be lost to overruns.
;
; This is the host-input hold: QEMU's chardev holds a burst when
; serial_can_receive says the device is full; this machine's Uart16550HostReceive
; does the same (4096 bytes, QEMU's CHR_READ_BUF_LEN). The wire path
; (PushRx, the loopback path) deliberately still overruns.
;
; The packet is the same one the movement in sermouse.asm produces:
;   6c 0a 36   left button, dx = +10, dy = -10
; and it repeats eight times.
;
; Build: nasm -f elf32 -o sermouse_burst.o sermouse_burst.asm
;        ld.lld -m elf_i386 -T sermouse.ld -o sermouse_burst.elf sermouse_burst.o
; Run:   cemu --machine x86 --isa x86 -mouse serial sermouse_burst.elf
;            (CEMU_DEBUG=mouse=10:-10:1@100000 x8)
; Exit: debug-exit 5 (status 11) when all 24 bytes arrived in order, 3 when the
;       host produced nothing, 1 on a mismatch.

  bits 32

%define DBGEXIT 0xf4
%define BUDGET  20000000    ; spin budget per byte (loop iterations)
%define NBYTES  24

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

  mov byte [nbytes], NBYTES
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
  mov ecx, NBYTES / 3
  mov esi, pkt
.next:
  mov al, [esi + 0]
  CHK 0x6c
  mov al, [esi + 1]
  CHK 0x0a
  mov al, [esi + 2]
  CHK 0x36
  add esi, 3
  loop .next
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

; COM1 with the receiver FIFO OFF: the one-deep receiver is what makes the host
; hold load-bearing. 1200 baud, 7 data bits, no parity, one stop bit, like the
; serial-mouse driver.
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
  xor al, al                   ; FCR: FIFO off
  out dx, al
  ret

exp:       db 0
got:       db 0
nokey:     db 0
nbytes:    db 0
pkt:       times NBYTES db 0
m_pkt:     db "pkt=", 0
m_ok:      db "sermouse-burst ok", 13, 10, 0
m_nohost:  db "sermouse-burst: no host event", 13, 10, 0
m_fail:    db "sermouse-burst FAIL exp=", 0
m_got:     db " got=", 0
