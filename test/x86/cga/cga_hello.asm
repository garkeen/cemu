; CGA display-window demo image (阶段 3.5 片 2, manual use): draws text, a
; 15-color attribute bar, a blinking-attribute cell and a hardware cursor,
; then parks — so `cemu --machine x86 --isa x86 -display win32 cga_hello.bin`
; keeps a window up for visual inspection. No debug-exit: the window close
; button ends the emulation. Semantics are the probe's job (cga_probe.asm).
;
; Build: nasm -f bin -o cga_hello.bin cga_hello.asm
  bits 16
  org 0x7c00

start:
  cli
  xor ax, ax
  mov ds, ax
  mov ss, ax
  mov sp, 0x7c00

  mov ax, 0xb800
  mov es, ax

  ; row 0: hello text, bright yellow on blue
  mov si, text
  xor di, di
  mov cx, text_len
.fill:
  lodsb
  mov ah, 0x1e
  stosw
  loop .fill

  ; row 2: fg colors 1..15 on black, block glyph
  mov di, 2 * 80 * 2
  mov ah, 0x01
  mov al, 0xdb
  mov cx, 15
.bar:
  stosw
  inc ah
  loop .bar

  ; row 4: '*' with a blinking attribute
  mov word [es:4 * 80 * 2], 0x8f2a

  ; hardware cursor at row 3, col 11 via the MC6845
  mov dx, 0x3d4
  mov al, 0x0f
  out dx, al
  mov dx, 0x3d5
  mov al, 80*3+11 & 0xff
  out dx, al
  mov dx, 0x3d4
  mov al, 0x0e
  out dx, al
  mov dx, 0x3d5
  mov al, (80*3+11) >> 8
  out dx, al

  ; park like real firmware: PIC master with only IRQ0 unmasked, PIT ch0
  ; periodic (divisor 0 = 65536), then STI/HLT — cemu's hlt stops the
  ; emulation unless it has a wake source, so the idle must be interruptible.
  mov dx, 0x20
  mov al, 0x11                     ; ICW1: cascade, ICW4 needed
  out dx, al
  mov dx, 0x21
  mov al, 0x08                     ; ICW2: vector base 0x08
  out dx, al
  mov al, 0x04                     ; ICW3: slave on IRQ2
  out dx, al
  mov al, 0x01                     ; ICW4: 8086 mode
  out dx, al
  mov al, 0xfe                     ; mask all but IRQ0
  out dx, al
  ; IRQ0 -> vector 8: install a bare iret so the park is a real idle loop
  ; (a zeroed IVT would send the interrupt into empty memory)
  mov word [8 * 4], irq0_stub
  mov word [8 * 4 + 2], 0
  mov dx, 0x43
  mov al, 0x36                     ; ch0, lo/hi, mode 3, binary
  out dx, al
  mov dx, 0x40
  xor al, al                       ; divisor 0 = 65536 (18.2 Hz)
  out dx, al
  out dx, al
  sti
.park:
  hlt
  jmp .park

irq0_stub:
  iret

text:    db "CGA display channel OK - 80x25 text"
text_len equ $ - text

  times 510-($-$$) db 0
  dw 0xaa55
