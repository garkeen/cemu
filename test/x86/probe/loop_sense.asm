; LOOP/LOOPE/LOOPNE/JCXZ and the INC/DEC flag contract -- the instruction mix a
; real-mode profile parser runs on (not an acceptance test).
;
; Intel SDM vol.2 fixes each of these, and each is easy to get subtly wrong in a
; way that only shows up inside a loop:
;   INC/DEC: "The CF flag is not affected" -- SF, ZF, AF, PF, OF are. A model
;     that clobbers CF breaks every borrow pattern downstream, and a model that
;     does not set ZF breaks every "dec cx / jz done" loop.
;   LOOP   (E2): CX = CX - 1, then jump if CX != 0.
;   LOOPE  (E1): jump if CX != 0 AND ZF = 1.
;   LOOPNE (E0): jump if CX != 0 AND ZF = 0.
;   JCXZ   (E3): jump if CX == 0 -- tested at the ADDRESS size.
;   The decrement happens before the test, so LOOP entered with CX = 1 falls
;   through, and LOOPNE stops both on a match and on a wrapped-out counter.
;
; This is the exact instruction mix of the WIN.INI canonicalizer in the Windows
; 1.01 kernel: it skips blanks with "lodsb / cmp al,0x20 / jz / cmp al,0x9 /
; loope", scans the key with "lodsb / stosb / dec cx / jz / cmp al,0x3d / jz /
; jcxz", and ends a line with "cmp al,0xa / jz". Nothing else is in its inner
; loops, so these five plus INC/DEC are what has to be right.
;
; Transcript per case: label, CX as four hex digits, then the captured flag byte
; (ZF bit6, PF bit4, CF bit0, SF bit7 plus whatever else the flags image held),
; then '!' when the case deviates from the SDM expectation stated on it.
; Dual run against qemu-system-i386 (same calibration as rep_zero.asm): the
; transcripts must match; the golden copy lives in loop_sense_qemu.txt.
;
;   a  stc; mov cx,1; inc cx     -> CX=0002, ZF=0, CF stays 1
;   b  clc; mov cx,1; dec cx     -> CX=0000, ZF=1, CF stays 0
;   c  scan "  xz" for a blank with the kernel's shape, cx=4
;                                -> stops on 'x': CX=0001, ZF=0, CF=0
;   d  loopne comparing 'z' against "  xz", cx=4, with "inc si" between the cmp
;     and the loopne -> ZF comes from INC, not from the compare: the loop runs
;     the full count and exits on CX = 0: CX=0000, ZF=0, CF=0, SF=1 (0x82)
;   e  mov cx,1; loop            -> falls through, CX=0000 (LOOP leaves flags)
;   f  xor cx,cx; jcxz           -> taken, CX=0000
;
; Build: nasm -f bin -o loop_sense.bin loop_sense.asm
; Run:   cemu --machine x86 --isa x86 loop_sense.bin
;        qemu-system-i386 -drive file=loop_sense.bin,format=raw,if=ide,index=0 \
;            -boot c -display none -serial stdio -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit value 7 on pass (QEMU status = (7<<1)|1 = 15), 1 on fail.

  bits 16
  org 0x7c00

%define STR     0x9000
%define DBGEXIT 0xf4

%macro VERIFY 3                ; expected CX, expected flag bits, mask
  cmp cx, %1
  jne %%bad
  mov al, [flb]
  and al, %3
  cmp al, %2
  je %%ok
%%bad:
  call set_fail
  mov al, '!'
  call putc
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
  call uart_init
  mov si, m_boot
  call puts
  mov byte [STR+0], ' '
  mov byte [STR+1], ' '
  mov byte [STR+2], 'x'
  mov byte [STR+3], 'z'

  ; a: INC must leave CF alone
  stc
  mov cx, 1
  inc cx
  call flags
  mov al, 'a'
  call show
  VERIFY 2, 0x01, 0x41

  ; b: DEC must leave CF alone, and must set ZF
  clc
  mov cx, 1
  dec cx
  call flags
  mov al, 'b'
  call show
  VERIFY 0, 0x40, 0x41

  ; c: the kernel's blank skip
  mov si, STR
  mov cx, 4
.c1:
  lodsb
  cmp al, ' '
  je .c2
  cmp al, 9
.c2:
  loope .c1
  call flags
  mov al, 'c'
  call show
  VERIFY 1, 0x00, 0x41

  ; d: LOOPNE over differing bytes -- and note the trap this case really tests:
  ; the "inc si" between the cmp and the loopne overwrites ZF (SDM INC: SF, ZF,
  ; AF, PF, OF are set from the result, CF is not), so LOOPNE never sees the
  ; comparison it appears to be looping on. The loop therefore runs the whole
  ; count and exits on CX = 0 with the last cmp being 'z' against the byte past
  ; the string. Both models must agree on that, because it is the SDM answer.
  mov si, STR
  mov al, 'z'
  mov cx, 4
.d1:
  cmp al, [si]
  inc si
  loopne .d1
  call flags
  mov al, 'd'
  call show
  VERIFY 0, 0x00, 0x41

  ; e: LOOP with CX = 1 falls through (the decrement is before the test)
  mov cx, 1
  loop .e1
  jmp .e2
.e1:
  call set_fail
.e2:
  call flags
  mov al, 'e'
  call show
  VERIFY 0, 0x00, 0x00

  ; f: JCXZ reads the counter and does not change it
  xor cx, cx
  jcxz .f1
  call set_fail
.f1:
  call flags
  mov al, 'f'
  call show
  VERIFY 0, 0x00, 0x00

  call crlf
  cmp byte [fail], 0
  jne bad
  mov al, 7
  jmp exit
bad:
  mov al, 1
exit:
  mov dx, DBGEXIT
  out dx, al
  hlt
  jmp $                       ; no debug-exit device: stop, never wrap

; ---- helpers ---------------------------------------------------------------

; Capture the flags image into [flb] without disturbing the flags themselves.
flags:
  pushf
  pop ax
  mov [flb], al
  ret

; AL = label; print it, then CX and the flag byte in hex.
show:
  push ax
  call putc
  pop ax
  mov al, ch
  call pethex
  mov al, cl
  call pethex
  mov al, [flb]
  call pethex
  mov al, ' '
  call putc
  ret

set_fail:
  mov byte [fail], 1
  ret

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

pethex:                       ; AL = byte -> two hex digits, AX and BX kept
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

fail:     db 0
flb:      db 0
m_boot:   db "loop-sense ", 0

  times 510-($-$$) db 0
  dw 0xaa55
