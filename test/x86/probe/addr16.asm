; 16-bit ModRM addressing: BP+SI / BP+DI and the 64K wrap of the offset.
;
; Intel SDM vol.1 table 2-1: rm=2 is [BP+SI], rm=3 is [BP+DI]; the effective
; offset is a 16-bit sum, so BP + disp16(-0x9c) wraps at 64K before the segment
; base is added (tiny386 modsib16 masks the sum with 0xffff). Both details are
; load-bearing for the Windows 1.01 display driver's font-name parser:
;   mov [bp+si-0x9c],al          ; 88 82 64 ff  (mod=2, disp16)
; A model that drops SI/DI from rm 2/3 or that adds disp16 as a 32-bit value
; writes 64K away from the stack buffer, the parser's name stays empty, and the
; kernel never finds its FONTS module. The probe uses the raw encoding because
; nasm would normally pick a disp8 for -0x9c.
;
; Transcript: label, the five bytes at SS:0928, two bytes at SS:0947.
; Expected on both models: 464f4e0000 4445.
;
; Build: nasm -f bin -o addr16.bin addr16.asm
; Run:   cemu --machine x86 --isa x86 addr16.bin
;        qemu-system-i386 -drive file=addr16.bin,format=raw,if=ide,index=0 \
;            -boot c -display none -serial stdio -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit value 5 on pass (QEMU status = (5<<1)|1 = 11), 1 on fail.

  bits 16
  org 0x7c00

%define DBGEXIT 0xf4

start:
  cli
  cld
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7c00
  call uart_init
  mov si, m_boot
  call puts

  mov ax, 0x1000               ; SS:0928 is the buffer the encoding targets
  mov ss, ax
  mov sp, 0x1000
  mov es, ax
  mov di, 0x920
  mov cx, 0x30
  xor al, al
  rep stosb

  mov bp, 0x9c4
  xor si, si
  mov al, 'F'
  db 0x88, 0x82, 0x64, 0xff    ; mov [bp+si-0x9c],al  (disp16, wraps to 0928)
  mov si, 1
  mov al, 'O'
  db 0x88, 0x82, 0x64, 0xff    ; 0929: the SI term must apply
  mov di, 2
  mov al, 'N'
  db 0x88, 0x83, 0x64, 0xff    ; 092a: BP+DI
  mov si, 3
  mov al, 'D'
  db 0x88, 0x42, 0x80          ; [bp+si-0x80] with disp8 -> 0947
  mov di, 4
  mov al, 'E'
  db 0x88, 0x43, 0x80          ; [bp+di-0x80] with disp8 -> 0948

  mov ax, 0x1000
  mov ds, ax
  mov si, 0x928
  mov cx, 5
.print:
  lodsb
  call pethex
  loop .print
  mov al, ' '
  call putc
  mov si, 0x947
  mov cx, 2
.print2:
  lodsb
  call pethex
  loop .print2
  call crlf

  cmp word [0x928], 0x4f46     ; "FO"
  jne bad
  cmp word [0x92a], 0x004e     ; "N\0"
  jne bad
  cmp word [0x947], 0x4544     ; "DE"
  jne bad
  mov al, 5
  jmp exit
bad:
  mov al, 1
exit:
  mov dx, DBGEXIT
  out dx, al
  hlt
  jmp $

; ---- UART helpers (same contract as loop_sense.asm) ------------------------
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
  push ax
  mov dx, 0x3fd
.wait:
  in al, dx
  test al, 0x20
  jz .wait
  pop ax
  mov dx, 0x3f8
  out dx, al
  ret

pethex:
  push ax
  push bx
  mov bl, al
  and al, 0xf0
  shr al, 4
  call nib
  mov al, bl
  and al, 0x0f
  call nib
  pop bx
  pop ax
  ret

nib:
  add al, '0'
  cmp al, '9'
  jbe .p
  add al, 7
.p:
  call putc
  ret

uart_init:
  mov dx, 0x3fb
  mov al, 0x80
  out dx, al
  mov dx, 0x3f8
  mov al, 0x01
  out dx, al
  xor al, al
  mov dx, 0x3f9
  out dx, al
  mov dx, 0x3fa
  out dx, al
  mov dx, 0x3fb
  mov al, 0x03
  out dx, al
  ret

m_boot: db "addr16 ", 0

  times 510-($-$$) db 0
  dw 0xaa55
