; REPE/REPNE sense probe for the CMPS/SCAS family (not an acceptance test).
;
; The repeat prefixes for CMPS/SCAS read the flags, and the two senses are exact
; opposites: one byte repeats while ZF = 1 (stop at the first mismatch -- how
; DOS and Windows compare a dotted name against a directory entry), the other
; repeats while ZF = 0 (stop at the first match -- how they find a terminator).
; A model that maps the two bytes onto the wrong sense terminates every name
; scan early or late: a control-flow divergence, not a slow one.
;
; The instructions below are written as raw bytes (`db 0xf2, 0xa6` and friends),
; because assembler conventions for the mnemonic spelling are exactly what is
; under investigation here: NASM assembles `repe cmpsb` to F3 A6 and `repne
; cmpsb` to F2 A6, while other tools disassemble those same bytes the other way
; round. Only the byte -> behaviour mapping is a fact about the machine, so that
; is what this probe measures: for each prefix byte it prints the counter left
; over (CX) and why the loop stopped (ZF), which together fix how many byte
; comparisons ran. MOVS is the control: the flag test must not reach it.
;
; Output is a fixed transcript, one field per case: `<label><CX 4 hex><ZF>`.
; Dual run against qemu-system-i386 (same calibration as rep_zero.asm): the
; transcripts must match; the golden copy lives in rep_sense_qemu.txt.
;
; Buffers (8 bytes apart, DS = ES = 0):
;   NEED 0x9000 "ABCDEF", HAY 0x9008 "ABDEFX" (equal at bytes 0-1, differ at 2),
;   OTHER 0x9010 "XYZWVU" (shares nothing with NEED), SCAN 0x9018 "abZZZZ",
;   COPYD 0x9040 (MOVS destination).
;
; Cases: 1 f2 cmpsb NEED/HAY cx=3 | 2 f3 cmpsb NEED/HAY cx=3 |
;        3 f3 cmpsb NEED/OTHER cx=3 | 4 f2 scasb AL='Z' SCAN cx=4 |
;        5 f3 scasb AL='Z' SCAN cx=4 | 6 f3 movsb NEED->COPYD cx=3, then the
;        two end bytes of the copy are checked and '!' printed if wrong.
;
; Build: nasm -f bin -o rep_sense.bin rep_sense.asm
; Run:   cemu --machine x86 --isa x86 rep_sense.bin
;        qemu-system-i386 -drive file=rep_sense.bin,format=raw,if=ide,index=0 \
;            -boot c -display none -serial stdio -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit value 6 on pass (QEMU status = (6<<1)|1 = 13), 1 on fail.

  bits 16
  org 0x7c00

%define NEED  0x9000           ; "ABCDEF"
%define HAY   0x9008           ; "ABDEFX"
%define OTHER 0x9010           ; "XYZWVU"
%define SCAN  0x9018           ; "abZZZZ"
%define COPYD 0x9040           ; movsb destination
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
  call fill

  mov byte [lab], '1'
  mov si, NEED
  mov di, HAY
  mov cx, 3
  db 0xf2, 0xa6               ; cmpsb, prefix F2
  call show

  mov byte [lab], '2'
  mov si, NEED
  mov di, HAY
  mov cx, 3
  db 0xf3, 0xa6               ; cmpsb, prefix F3
  call show

  mov byte [lab], '3'
  mov si, NEED
  mov di, OTHER
  mov cx, 3
  db 0xf3, 0xa6               ; cmpsb over operands with nothing in common
  call show

  mov byte [lab], '4'
  mov di, SCAN
  mov al, 'Z'
  mov cx, 4
  db 0xf2, 0xae               ; scasb AL vs ES:DI, prefix F2
  call show

  mov byte [lab], '5'
  mov di, SCAN
  mov al, 'Z'
  mov cx, 4
  db 0xf3, 0xae               ; scasb AL vs ES:DI, prefix F3
  call show

  mov byte [lab], '6'
  mov si, NEED
  mov di, COPYD
  mov cx, 3
  db 0xf3, 0xa4               ; movsb, prefix F3: the control
  call show
  mov al, [COPYD]
  cmp al, 'A'
  je .f2
  call set_fail
.f2:
  mov al, [COPYD+2]
  cmp al, 'C'
  je .f3
  call set_fail
.f3:
  call crlf
  cmp byte [fail], 0
  jne bad
  mov al, 6
  jmp exit
bad:
  mov al, 1
exit:
  mov dx, DBGEXIT
  out dx, al
  hlt
  jmp $                       ; no debug-exit device: stop, never wrap

; ---- helpers ---------------------------------------------------------------

; Lay the four name buffers out at NEED..SCAN. A plain loop, not a prefixed one:
; the buffers must be trustworthy whatever the repeat prefixes do.
fill:
  mov si, tmpl
  mov di, NEED
  mov cx, 32
.f:
  movsb
  loop .f
  ret

; BP = case label: print it, then CX as four hex digits and ZF as one.
show:
  push ax
  pushf
  pop bx
  and bl, 0x40                ; ZF is bit 6 of the flags image
  shr bl, 6
  mov al, [lab]
  call putc
  mov al, ch
  call pethex
  mov al, cl
  call pethex
  mov al, bl
  or al, '0'
  call putc
  pop ax
  ret

set_fail:
  mov byte [fail], 1
  ret

puts:                         ; SI = NUL-terminated string
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

putc:                         ; AL = character, waits for THRE
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

; AL = byte -> two hex digits; AX and BX preserved.
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

nib:                          ; AL = 0..15
  add al, '0'
  cmp al, '9'
  jbe .p
  add al, 7
.p:
  call putc
  ret

uart_init:
  mov dx, 0x3fb               ; LCR: divisor latch
  mov al, 0x80
  out dx, al
  mov dx, 0x3f8               ; DLL = 1 (115200 baud)
  mov al, 0x01
  out dx, al
  mov dx, 0x3f9               ; DLH = 0
  xor al, al
  out dx, al
  mov dx, 0x3fa               ; IER = 0
  out dx, al
  mov dx, 0x3fb               ; 8N1, DLAB = 0
  mov al, 0x03
  out dx, al
  ret

fail:     db 0
lab:      db 0
m_boot:   db "rep-sense ", 0
tmpl:     db "ABCDEF", 0, 0, "ABDEFX", 0, 0, "XYZWVU", 0, 0, "abZZZZ", 0, 0

  times 510-($-$$) db 0
  dw 0xaa55
