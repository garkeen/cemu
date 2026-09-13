; CGA text-mode smoke probe for the x86_min machine (阶段 3.5 片 2; not an
; acceptance test — acceptance is kvm-unit-tests). Loaded at 0x7C00 with
; CS:IP = 0000:7C00 and DL=0x80.
;
; Headless-verified semantics (COM1 + debug-exit):
;   1. VRAM write/readback through 0xB8000 with attribute bytes
;   2. MC6845 cursor address (R14/R15) program + readback via 0x3D4/0x3D5
;   3. a 0x3DA vertical-retrace edge (status bit 3: 0 -> 1) within a bound
; Visual part (needs -display win32, judged by eye): row 0 text, a 15-color
; attribute bar on row 2, a blinking-attribute cell on row 4 and the
; hardware cursor at row 3 col 11.
;
; Exit: payload 5 -> emulator status (5<<1)|1 = 11; failure -> payload 2
; -> status 5 after printing "cga-probe BAD <step>".
;
; Build: nasm -f bin -o cga_probe.bin cga_probe.asm
  bits 16
  org 0x7c00

kCrtIndex equ 0x3d4
kCrtData  equ 0x3d5
kCgaStat  equ 0x3da
kCurCell  equ 80*3+11          ; cursor target: row 3, col 11 (80-col text)

start:
  cli
  xor ax, ax
  mov ds, ax
  mov ss, ax
  mov sp, 0x7c00

  ; COM1 init: divisor 1 (115200 baud), 8N1, interrupts off
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

  ; ---- 1. VRAM write + readback (row 0, attr 0x1E) ----
  mov ax, 0xb800
  mov es, ax
  mov si, text
  xor di, di
  mov cx, text_len
.fill:
  lodsb
  mov ah, 0x1e
  stosw
  loop .fill
  mov si, text
  xor di, di
  mov cx, text_len
.check:
  lodsb
  cmp al, [es:di]
  jne bad1
  cmp byte [es:di+1], 0x1e
  jne bad1
  add di, 2
  loop .check

  ; ---- 2. cursor address: program R15/R14 then read back ----
  mov dx, kCrtIndex
  mov al, 0x0f                     ; R15: cursor address low
  out dx, al
  mov dx, kCrtData
  mov al, kCurCell & 0xff
  out dx, al
  mov dx, kCrtIndex
  mov al, 0x0e                     ; R14: cursor address high
  out dx, al
  mov dx, kCrtData
  mov al, kCurCell >> 8
  out dx, al
  mov dx, kCrtIndex
  mov al, 0x0e
  out dx, al
  mov dx, kCrtData
  in al, dx
  mov bh, al                       ; R14 = high byte
  mov dx, kCrtIndex
  mov al, 0x0f
  out dx, al
  mov dx, kCrtData
  in al, dx
  mov bl, al                       ; R15 = low byte
  cmp bx, kCurCell
  jne bad2

  ; ---- 3. vertical retrace edge: status bit 3, 0 -> 1, bounded ----
  mov dx, kCgaStat
  mov bp, 0x10
.outer0:
  mov cx, 0xffff
.inner0:
  in al, dx
  test al, 0x08
  jz .got0
  loop .inner0
  dec bp
  jnz .outer0
  jmp bad3
.got0:
  mov bp, 0x10
.outer1:
  mov cx, 0xffff
.inner1:
  in al, dx
  test al, 0x08
  jnz .got1
  loop .inner1
  dec bp
  jnz .outer1
  jmp bad3
.got1:

  ; ---- visual extras (no headless judge): color bar + blink cell ----
  mov di, 2 * 80 * 2               ; row 2
  mov ah, 0x01                     ; fg 1..15 on black, block glyph
  mov al, 0xdb
  mov cx, 15
.bar:
  stosw
  inc ah
  loop .bar
  mov word [es:4 * 80 * 2], 0x8f2a ; row 4: '*' with a blinking attribute

  mov si, okmsg
.print:
  lodsb
  test al, al
  jz .done
  call putc
  jmp .print
.done:
  mov al, 5                        ; exit payload: status = (5<<1)|1
  mov dx, 0xf4
  out dx, al
.hang:
  hlt
  jmp .hang

bad1:
  mov si, bad_vram
  jmp fail
bad2:
  mov si, bad_crtc
  jmp fail
bad3:
  mov si, bad_vsync
fail:
  call puts
  mov al, 2                        ; exit payload: status = (2<<1)|1 = 5
  mov dx, 0xf4
  out dx, al
.fhang:
  hlt
  jmp .fhang

putc:                              ; AL = character, waits for THRE
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

puts:                              ; SI = $-terminated string (DS = 0)
  lodsb
  test al, al
  jz .done
  call putc
  jmp puts
.done:
  ret

text:     db "CGA-PROBE"
text_len  equ $ - text
okmsg:    db "cga-probe ok", 13, 10, 0
bad_vram: db "cga-probe BAD vram", 13, 10, 0
bad_crtc: db "cga-probe BAD crtc", 13, 10, 0
bad_vsync: db "cga-probe BAD vsync", 13, 10, 0

  times 510-($-$$) db 0
  dw 0xaa55
