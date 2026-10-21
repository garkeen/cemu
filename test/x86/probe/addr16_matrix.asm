; Complete 16-bit ModRM effective-address matrix (SDM vol.1 table 2-1):
; the eight r/m forms under mod=0/1/2 and the default segment of each.
;
; DS, SS and ES have different bases, so a wrong default segment writes the
; marker to a different physical address than the one read back:
;   DS=0x1000 (base 0x10000), SS=0x2000 (base 0x20000),
;   ES=0x3000 (base 0x30000). The 0..0x3FFF window in each is cleared first,
; so a marker that lands somewhere else reads as zero.
;
; The table-driven runner stores a unique marker through each EA, then reads
; the expected segment at the expected offset with a plain [es:di] load. The
; EA snippets read DS default for r/m 0,1,4,5,6(mod=0),7 and SS default for
; r/m 2,3 and 6(mod=1/2); the read segment is the table's DS or SS.
;
; Output is the isa-debugcon port (0x402), the two-model byte stream from
; AGENTS.md section 10, so the 512-byte sector fits without a UART driver.
;
; Run (self-contained; no firmware):
;   cemu --machine x86 --isa x86 addr16_matrix.bin
;   qemu-system-i386 -machine pc -m 32 -drive file=addr16_matrix.bin,format=raw,if=floppy \
;       -boot a -display none -no-reboot \
;       -chardev file,id=dbg,path=addr16_matrix_qemu.txt \
;       -device isa-debugcon,iobase=0x402,chardev=dbg \
;       -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit 5 (status 11) on pass, 1 (status 3) on the first mismatch;
; the failing case number is printed in hex.

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

  mov ax, 0x1000
  mov ds, ax
  mov ax, 0x2000
  mov ss, ax
  mov sp, 0x7c00
  mov ax, 0x3000
  mov es, ax

  ; Clear the tested window in each data segment. The stack is at SS:7C00.
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

  mov bx, 0x1000
  mov bp, 0x2000
  mov si, 0x0300
  mov di, 0x0400

  ; DS-default cases, then SS-default cases.
  push ds
  pop es
  mov si, table_ds
  call run_table
  push ss
  pop es
  mov si, table_ss
  call run_table

  push cs
  pop ds
  mov si, msg_ok
  call puts
  call crlf
  mov al, 5
  jmp exit

; SI = table, ES = segment to verify. Each record is
; { code:dw, expected offset:dw, marker:dw }, terminated by code=0.
run_table:
.loop:
  mov ax, [cs:si]
  test ax, ax
  jz .done
  mov dl, [cs:si+4]
  push si
  mov si, 0x0300
  call ax
  pop si
  push di
  mov di, [cs:si+2]
  mov dh, [es:di]
  pop di
  cmp dh, dl
  je .next
  mov al, dl
  jmp fail
.next:
  add si, 6
  jmp .loop
.done:
  ret

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

; ---- EA snippets: store DL through the form under test ----------------------
case1:  mov [bx+si], dl
        ret
case2:  mov [bx+di], dl
        ret
case3:  mov [bp+si], dl
        ret
case4:  mov [bp+di], dl
        ret
case5:  mov [si], dl
        ret
case6:  mov [di], dl
        ret
case7:  mov [0x0500], dl
        ret
case8:  mov [bx], dl
        ret
case9:  mov [bx+si+0x10], dl
        ret
case10: mov [bx+di+0x10], dl
        ret
case11: mov [bp+si+0x10], dl
        ret
case12: mov [bp+di+0x10], dl
        ret
case13: mov [si+0x10], dl
        ret
case14: mov [di+0x10], dl
        ret
case15: mov [bp+0x10], dl
        ret
case16: mov [bx+0x10], dl
        ret
case17: mov [bx+si+0x1234], dl
        ret
case18: mov [bx+di+0x1234], dl
        ret
case19: mov [bp+si+0x1234], dl
        ret
case20: mov [bp+di+0x1234], dl
        ret
case21: mov [si+0x1234], dl
        ret
case22: mov [di+0x1234], dl
        ret
case23: mov [bp+0x1234], dl
        ret
case24: mov [bx+0x1234], dl
        ret

; ---- Tables ----------------------------------------------------------------
; The eight DS-default forms repeat in each mod, as do the three SS-default
; r/m forms (2, 3 and 6 with a displacement).
table_ds:
  dw case1,  0x1300, 1
  dw case2,  0x1400, 2
  dw case5,  0x0300, 5
  dw case6,  0x0400, 6
  dw case7,  0x0500, 7
  dw case8,  0x1000, 8
  dw case9,  0x1310, 9
  dw case10, 0x1410, 10
  dw case13, 0x0310, 13
  dw case14, 0x0410, 14
  dw case16, 0x1010, 16
  dw case17, 0x2534, 17
  dw case18, 0x2634, 18
  dw case21, 0x1534, 21
  dw case22, 0x1634, 22
  dw case24, 0x2234, 24
  dw 0

table_ss:
  dw case3,  0x2300, 3
  dw case4,  0x2400, 4
  dw case11, 0x2310, 11
  dw case12, 0x2410, 12
  dw case15, 0x2010, 15
  dw case19, 0x3534, 19
  dw case20, 0x3634, 20
  dw case23, 0x3234, 23
  dw 0

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

msg_ok:   db "addr16m ok", 0
msg_fail: db "addr16m FAIL case 0x", 0

  times 510 - ($ - $$) db 0
  dw 0xaa55
