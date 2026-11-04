; Sound Blaster 16, 8-bit DMA playback, driven by the guest alone (not an
; acceptance test -- a probe that pins the DSP/mixer/8237 handshake).
;
; The card is QEMU's sb16 model (iobase 0x220, IRQ5, DMA1/DMA5); this probe
; programs the DSP and the 8237 itself, then checks the three guest-visible
; results of one single-cycle block:
;
;   irq   mixer 0x82 after the block: bit 0 (8-bit IRQ latch) set
;   tc    8237 status register: channel 1 reached terminal count
;   cnt   8237 count register after the block: 0xffff (the controller spent it)
;   ack   mixer 0x82 after reading 0x22e: the latch is gone
;
; The buffer is filled with 0x80, the unsigned 8-bit midpoint, so even a run
; with -audio win32 plays silence. Host output is off by default.
;
; Reference: QEMU hw/audio/sb16.c (command 0x14 via dma_cmd8, the IRQ-status
; byte at mixer 0x82, the 0x22e acknowledge) and hw/dma/i8257.c (status/TC,
; count register); Intel 8237A datasheet (count holds N-1, TC status bit).
;
; Build: nasm -f elf32 -o sb16_dma8.o sb16_dma8.asm
;        ld.lld -m elf_i386 -T vbe_mode.ld -o sb16_dma8.elf sb16_dma8.o
; Run:   cemu --machine x86 --isa x86 sb16_dma8.elf
;        qemu-system-i386 -machine pc -m 32 -kernel sb16_dma8.elf \
;            -display none -no-reboot -audiodev none,id=snd0 \
;            -device sb16,audiodev=snd0 \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4 \
;            -serial file:sb16_dma8_qemu.txt
; Exit: debug-exit 5 (status 11) once the transcript is out.

  bits 32

%define SB_BASE       0x220
%define SB_RESET      0x226
%define SB_READ       0x22a
%define SB_WRITE      0x22c
%define SB_DATA       0x22e       ; 0x80 = DSP data available; reading acks IRQ8
%define SB_MIX_ADDR   0x224
%define SB_MIX_DATA   0x225

%define DMA_ADDR      0x02        ; channel 1 address
%define DMA_COUNT     0x03        ; channel 1 word count
%define DMA_STATUS    0x08
%define DMA_MODE      0x0b
%define DMA_CLEAR     0x0c
%define DMA_MASK      0x0a
%define DMA_PAGE      0x83        ; channel 1 page latch

%define DMA_FROM_MEM  0x09        ; single mode, channel 1, memory -> device
%define TC_CH1        0x02

%define BUF           0x20000
%define LEN           64
%define DBGEXIT       0xf4

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

  ; --- DSP reset: 1 then 0 on 0x226, then the 0xaa identification byte -------
  mov dx, SB_RESET
  mov al, 1
  out dx, al
  mov ecx, 20000
  call delay
  xor al, al
  out dx, al
  mov ecx, 20000
  call delay
  call dsp_wait_data
  mov dx, SB_READ
  in al, dx
  cmp al, 0xaa
  jne fail

  ; Speaker on (0xd1): playback runs with it off too, but the guest turns it
  ; on before a transfer, and QEMU's dma_cmd8 does the same.
  mov al, 0xd1
  call dsp_write

  ; --- one block of silence in guest memory ----------------------------------
  mov edi, BUF
  mov ecx, LEN
  mov al, 0x80
  rep stosb

  ; --- 8237 channel 1: address, count, page, mode, unmask --------------------
  mov dx, DMA_CLEAR
  xor al, al
  out dx, al
  mov dx, DMA_ADDR
  mov al, BUF & 0xff
  out dx, al
  mov al, (BUF >> 8) & 0xff
  out dx, al
  mov dx, DMA_CLEAR
  xor al, al
  out dx, al
  mov dx, DMA_COUNT
  mov al, (LEN - 1) & 0xff
  out dx, al
  mov al, (LEN - 1) >> 8
  out dx, al
  mov dx, DMA_PAGE
  mov al, (BUF >> 16) & 0xff
  out dx, al
  mov dx, DMA_MODE
  mov al, DMA_FROM_MEM
  out dx, al
  mov dx, DMA_MASK
  mov al, 0x01              ; channel 1, clear the mask bit
  out dx, al

  ; --- command 0x14: single-cycle 8-bit DMA, length-1 in low/high order ------
  mov al, 0x14
  call dsp_write
  mov al, (LEN - 1) & 0xff
  call dsp_write
  mov al, (LEN - 1) >> 8
  call dsp_write

  ; --- wait for the block's IRQ latch ---------------------------------------
  mov bl, 0x01
  call wait_irq

  mov al, 0x82
  call mixer_read
  mov [irq], al

  ; Acknowledge through 0x22e, then read the latch back: it must be gone.
  mov dx, SB_DATA
  in al, dx
  mov al, 0x82
  call mixer_read
  mov [ack], al

  ; --- 8237 results ----------------------------------------------------------
  mov dx, DMA_STATUS
  in al, dx
  mov [tc], al

  mov dx, DMA_CLEAR
  xor al, al
  out dx, al
  mov dx, DMA_COUNT
  in al, dx
  mov [cnt_lo], al
  in al, dx
  mov [cnt_hi], al

  ; --- transcript ------------------------------------------------------------
  P m_tag
  mov al, [irq]
  call hex8
  P m_tc
  mov al, [tc]
  call hex8
  P m_cnt
  mov al, [cnt_hi]
  call hex8
  mov al, [cnt_lo]
  call hex8
  P m_ack
  mov al, [ack]
  call hex8
  P m_ok
  P m_crlf
  mov dx, DBGEXIT
  mov al, 5
  out dx, al
  jmp $

fail:
  P m_fail
  mov dx, DBGEXIT
  mov al, 1
  out dx, al
  jmp $

; === DSP ====================================================================
dsp_write:                      ; AL = command/data byte
  push edx
  mov dx, SB_WRITE
  out dx, al
  pop edx
  ret

; Wait for the DSP's output buffer to hold a byte. Reading 0x22e also
; acknowledges the 8-bit IRQ latch, so this is only called before a transfer
; (the reset identification byte).
dsp_wait_data:
  push ecx
  push edx
  mov ecx, 2000000
.wait:
  mov dx, SB_DATA
  in al, dx
  test al, 0x80
  jnz .done
  dec ecx
  jnz .wait
  mov al, 0
  pop edx
  pop ecx
  ret
.done:
  pop edx
  pop ecx
  ret

mixer_read:                     ; AL = register index -> AL = value
  push edx
  mov dx, SB_MIX_ADDR
  out dx, al
  mov dx, SB_MIX_DATA
  in al, dx
  pop edx
  ret

; Poll mixer 0x82 until the requested latch (BL) is set. The mixer read does
; not acknowledge anything; 0x22e/0x22f are the ack ports.
wait_irq:
  push ecx
  mov ecx, 20000000
.wait:
  mov al, 0x82
  call mixer_read
  test al, bl
  jnz .done
  dec ecx
  jnz .wait
  pop ecx
  jmp fail
.done:
  pop ecx
  ret

delay:                          ; ECX iterations
  dec ecx
  jnz delay
  ret

; === output =================================================================
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
irq:      db 0
ack:      db 0
tc:       db 0
cnt_lo:   db 0
cnt_hi:   db 0
m_tag:    db "sb16-dma8 irq=", 0
m_tc:     db " tc=", 0
m_cnt:    db " cnt=", 0
m_ack:    db " ack=", 0
m_ok:     db " ok", 0
m_fail:   db "sb16-dma8 FAIL", 13, 10, 0
m_crlf:   db 13, 10, 0
