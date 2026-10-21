; 16-bit addressing: segment override, sign-extended disp8 and the 64K wrap.
; The companion addr16_matrix.asm covers the 24 r/m x mod forms and their
; default segments; this probe pins the prefix behaviour and the arithmetic
; edge cases the table cannot express.
;
; DS=0x1000, SS=0x2000, ES=0x3000, CS=0 (the boot sector is at 0:7C00). The
; tested window 0..0x3FFF is cleared in each data segment and CS:0500 is
; cleared too, so a miss reads zero. Cases:
;   1-4  ES/DS/SS/CS override on a BP+SI or BX+SI access at offset 0500
;   5-6  negative disp8, DS and SS defaults
;   7-9  BP+SI, BX+disp16 and BP+disp8 wrap at 64K
;   10-12 the Windows 1.01 encoding [bp+si-0x9c]: a negative disp16 is a
;         plain 16-bit displacement, and BP+SI wraps before the SS base
;
; Output is the isa-debugcon port 0x402 (AGENTS.md section 10).
; Run (self-contained; no firmware):
;   cemu --machine x86 --isa x86 addr16_seg.bin
;   qemu-system-i386 -machine pc -m 32 -drive file=addr16_seg.bin,format=raw,if=floppy \
;       -boot a -display none -no-reboot \
;       -chardev file,id=dbg,path=addr16_seg_qemu.txt \
;       -device isa-debugcon,iobase=0x402,chardev=dbg \
;       -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit 5 (status 11) on pass, 1 (status 3) on the first mismatch.

  bits 16
  org 0x7c00

%define DBGEXIT 0xf4

%macro VERIFY 4
  mov dh, [%2:%3]
  cmp dh, %1
  je %%ok
  mov al, %4
  jmp fail
%%ok:
%endmacro

start:
  cli
  cld
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7c00

  mov ax, 0x1000
  mov ds, ax
  mov ax, 0x2000
  mov ss, ax
  mov sp, 0x7c00
  mov ax, 0x3000
  mov es, ax

  mov ax, ds
  mov es, ax
  xor di, di
  xor ax, ax
  mov cx, 0x2000
  rep stosw
  mov ax, ss
  mov es, ax
  xor di, di
  xor ax, ax
  mov cx, 0x2000
  rep stosw
  mov ax, 0x3000
  mov es, ax
  xor di, di
  xor ax, ax
  mov cx, 0x2000
  rep stosw
  mov word [cs:0x0500], 0

  ; 1-4: the same effective offset, only the prefix selects the segment.
  mov bx, 0x0400
  mov bp, 0x0400
  mov si, 0x0100
  mov dl, 1
  mov es:[bp+si], dl
  VERIFY 1, es, 0x0500, 1
  mov dl, 2
  mov ds:[bp+si], dl
  VERIFY 2, ds, 0x0500, 2
  mov dl, 3
  mov ss:[bx+si], dl
  VERIFY 3, ss, 0x0500, 3
  mov dl, 4
  mov cs:[bx+si], dl
  VERIFY 4, cs, 0x0500, 4

  ; 5-6: disp8 is sign-extended before the 16-bit sum.
  mov bx, 0x1300
  xor si, si
  mov dl, 5
  mov [bx+si-0x10], dl
  VERIFY 5, ds, 0x12f0, 5
  mov bp, 0x2000
  mov di, 0x0100
  mov dl, 6
  mov [bp+di-0x20], dl
  VERIFY 6, ss, 0x20e0, 6

  ; 7-9: the carry out of the 16-bit sum is dropped.
  mov bp, 0xff00
  mov si, 0x0200
  mov dl, 7
  mov [bp+si], dl
  VERIFY 7, ss, 0x0100, 7
  mov bx, 0xff00
  mov dl, 8
  mov [bx+0x0200], dl
  VERIFY 8, ds, 0x0100, 8
  mov bp, 0xfff0
  mov dl, 9
  mov [bp+0x20], dl
  VERIFY 9, ss, 0x0010, 9

  ; 10-12: the exact encoding Windows 1.01's font-name parser uses. disp16
  ; 0xFF64 is not sign-extended; it is added as a 16-bit value, and the
  ; BP+SI/BP+DI sum wraps at 64K before the SS base is added.
  mov bp, 0x09c4
  xor si, si
  mov dl, 10
  mov [bp+si-0x9c], dl
  VERIFY 10, ss, 0x0928, 10
  mov si, 1
  mov dl, 11
  mov [bp+si-0x9c], dl
  VERIFY 11, ss, 0x0929, 11
  mov di, 2
  mov dl, 12
  mov [bp+di-0x9c], dl
  VERIFY 12, ss, 0x092a, 12

  push cs
  pop ds
  mov si, msg_ok
  call puts
  call crlf
  mov al, 5
  jmp exit

fail:
  push ax
  push cs
  pop ds
  mov si, msg_fail
  call puts
  pop ax
  call puthex8
  call crlf
  mov al, 1

exit:
  mov dx, DBGEXIT
  out dx, al
  hlt
  jmp $

; ---- debugcon helpers (port 0x402) -----------------------------------------
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
  mov dx, 0x402
  out dx, al
  ret

puthex8:
  push ax
  shr al, 4
  call .nib
  pop ax
  and al, 0x0f
.nib:
  cmp al, 10
  jb .digit
  add al, 'a' - 10
  jmp .emit
.digit:
  add al, '0'
.emit:
  call putc
  ret

msg_ok:   db "addr16s ok", 0
msg_fail: db "addr16s FAIL case 0x", 0

  times 510 - ($ - $$) db 0
  dw 0xaa55
