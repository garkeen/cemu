; VGA family core probe: the card is driven the way real software drives it —
; through the VGA BIOS's int10 services, not by poking registers by hand — and
; then through the register file the BIOS leaves behind. Boot sector on A:
; (SeaBIOS and the display card's option ROM must both be present).
;
; Checks (COM1 + debug-exit; QEMU runs the same probe for the A/B):
;   1. int10 AX=0x0003 (80x25 text): the CRTC ends up as the BIOS's mode-3
;      table says - offset R13 = 0x28 (40 words = 80 columns), R9 = 0x4f
;      (16-line cells), vertical display end 399.
;   2. int10 AX=0x0010 (EGA 640x350, the mode Windows 1.01's display driver
;      asks for): R9 = 0x40, R13 = 0x28 (80 bytes a row), display end 349, the
;      attribute controller in graphics mode with all four planes enabled, and
;      the graphics controller in planar packing (GR5 shift control = 0).
;   3. Plane addressing: with the sequencer's plane mask selecting one plane at
;      a time, a byte written to A0000 goes to that plane only and the read map
;      select register reads it back - the path every 16-colour picture uses.
;   4. int10 AX=0x0004 (CGA 320x200): GR5 shift control = 1 and the sequencer's
;      2:1 dot clock, i.e. the packed 2bpp shape.
;
; Exit: payload 5 -> status (5<<1)|1 = 11; a failed check prints
; "vga-probe BAD <the value the card read back>" and exits with payload 2
; -> status 5.
;
; Build: nasm -f bin -o vga_probe.bin vga_probe.asm
  bits 16
  org 0x7c00

kAttrIndex equ 0x3c0
kStatus1   equ 0x3da
kSeqIndex  equ 0x3c4
kGfxIndex  equ 0x3ce
kCrtIndex  equ 0x3d4
kCrtData   equ 0x3d5
kGraphSeg  equ 0xa000

start:
  cli
  cld
  xor ax, ax
  mov ds, ax
  mov ss, ax
  mov sp, 0x7c00
  call uart_init

  ; ---- 1. int10 AX=0x0003: the mode-3 geometry ----
  mov ax, 0x0003
  int 0x10
  mov al, 0x13                     ; R13: logical line width in words
  call crtc_read
  cmp al, 0x28                     ; 40 words = 80 columns (stdvga crtc_03)
  jne bad1
  mov al, 0x09                     ; R9: maximum scan line
  call crtc_read
  cmp al, 0x4f                     ; 16-line cells
  jne bad1
  call vde_read                    ; vertical display end
  cmp al, 0x8f                     ; 25 rows x 16 lines - 1: 399 = 0x18f
  jne bad1
  cmp ah, 0x01
  jne bad1

  ; ---- 2. int10 AX=0x0010: EGA 640x350, planar ----
  mov ax, 0x0010
  int 0x10
  mov al, 0x09
  call crtc_read
  cmp al, 0x40                     ; one scan line per row (stdvga crtc_0f)
  jne bad2
  mov al, 0x13
  call crtc_read
  cmp al, 0x28                     ; 40 words = 80 bytes a row = 640 pixels
  jne bad2
  call vde_read
  cmp al, 0x5d                     ; 350 lines: 349 = 0x15d
  jne bad2
  cmp ah, 0x01
  jne bad2
  mov dx, kGfxIndex                ; GR5: the shift control must be planar (0)
  mov al, 0x05
  out dx, al
  inc dx
  in al, dx
  and al, 0x60
  jnz bad2
  mov dx, kStatus1                 ; reading status 1 resets the attribute
  in al, dx                        ; controller's index/data flip-flop, which
  mov dx, kAttrIndex               ; the BIOS's own sequence left set: the
  mov al, 0x10                     ; next 0x3C0 write would land as data
  out dx, al
  inc dx                           ; 0x3C1 is the attribute data port
  in al, dx
  test al, 0x01
  jz bad2
  mov dx, kStatus1                 ; same dance for the plane-enable register
  in al, dx
  mov dx, kAttrIndex
  mov al, 0x12
  out dx, al
  inc dx
  in al, dx
  cmp al, 0x0f
  jne bad2

  ; ---- 3. plane addressing through the aperture ----
  ; Start from a known state: one zero byte in all four planes.
  mov dx, kSeqIndex                ; SR2 = the plane write mask
  mov al, 0x02
  out dx, al
  inc dx
  mov al, 0x0f                     ; all four planes
  out dx, al
  mov ax, kGraphSeg
  mov es, ax
  mov byte [es:0], 0x00
  xor bx, bx                       ; BL = the plane under test
.plane:
  mov dx, kSeqIndex                ; SR2 = this plane only
  mov al, 0x02
  out dx, al
  inc dx
  mov al, 1
  mov cl, bl
  shl al, cl
  out dx, al
  mov dx, kGfxIndex                ; GR4 = read map select: the same plane
  mov al, 0x04
  out dx, al
  inc dx
  mov al, bl
  out dx, al
  mov al, [es:0]                   ; untouched so far: the previous plane's
  test al, al                      ; write must not have reached this one
  jnz bad3
  mov byte [es:0], 0x5a            ; the pattern lands in that plane alone
  mov al, [es:0]                   ; and reads back out of it
  cmp al, 0x5a
  jne bad3
  inc bx
  cmp bx, 4
  jb .plane

  ; ---- 4. int10 AX=0x0004: the CGA 320x200 shape ----
  mov ax, 0x0004
  int 0x10
  mov dx, kGfxIndex                ; GR5: shift control 1 (2bpp packed)
  mov al, 0x05
  out dx, al
  inc dx
  in al, dx
  and al, 0x60
  cmp al, 0x20
  jne bad4
  mov dx, kSeqIndex                ; SR1 bit 3: the 2:1 dot clock
  mov al, 0x01
  out dx, al
  inc dx
  in al, dx
  test al, 0x08
  jz bad4

  mov si, okmsg
  call puts
  mov al, 5                        ; exit payload: (5<<1)|1 = 11
  mov dx, 0xf4
  out dx, al
.hang:
  hlt
  jmp .hang

; --- helpers ---------------------------------------------------------------
; crtc_read: AL = register index -> AL = its value (through 0x3D4/0x3D5)
crtc_read:
  mov dx, kCrtIndex
  out dx, al
  mov dx, kCrtData
  in al, dx
  ret

; vde_read: AX = the vertical display end, CRTC R18 with R7's bits 1 and 6
; carrying bits 8 and 9 (IBM VGA CRTC overflow register).
vde_read:
  mov al, 0x12
  call crtc_read
  mov bl, al                       ; bits 0-7
  mov al, 0x07
  call crtc_read
  mov bh, al                       ; R7
  xor ax, ax
  mov al, bl
  test bh, 0x02
  jz .no_bit8
  or ax, 0x100
.no_bit8:
  test bh, 0x40
  jz .no_bit9
  or ax, 0x200
.no_bit9:
  ret

; puts: SI = $-terminated string (DS = 0) on COM1
puts:
  lodsb
  test al, al
  jz .done
  call putc
  jmp puts
.done:
  ret

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

uart_init:                         ; COM1: divisor 1 (115200), 8N1, no interrupts
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

; A failed check prints the value it read, so a run says what the card gave
; back rather than only which step tripped.
bad1: jmp fail                     ; mode-3 R13 / R9 / display end
bad2: jmp fail                     ; EGA 640x350 geometry or registers
bad3: jmp fail                     ; plane write/read-back
bad4: jmp fail                     ; CGA 320x200 shape
fail:
  mov si, badmsg
  call puts
  call print_hex8                  ; AL = the value that was wrong
  mov al, 13
  call putc
  mov al, 10
  call putc
  mov al, 2                        ; exit payload: (2<<1)|1 = 5
  mov dx, 0xf4
  out dx, al
.fhang:
  hlt
  jmp .fhang

print_hex8:                        ; AL = byte, as two hex digits
  push ax
  push bx
  mov bl, al
  shr al, 1
  shr al, 1
  shr al, 1
  shr al, 1
  call print_nib
  mov al, bl
  and al, 0x0f
  call print_nib
  pop bx
  pop ax
  ret

print_nib:                         ; AL = 0..15 -> one hex digit on COM1
  add al, '0'
  cmp al, '9' + 1
  jb .emit
  add al, 'a' - '0' - 10
.emit:
  jmp putc                         ; tail call: its ret returns to our caller

okmsg: db "vga-probe ok", 13, 10, 0
badmsg: db "vga-probe BAD ", 0

  times 510-($-$$) db 0
  dw 0xaa55

