; The register file's answers: what a guest reads back from the VGA's port block
; (not an acceptance test).
;
; A display driver identifies the card by writing patterns into the register file
; and reading them back -- the latch widths, the write masks, which half of the
; port block decodes, the status bits -- and it then decides which video modes it
; may ask for. Two emulators whose register file answers differently get two
; different guests: same firmware, same disk, different branch. This probe is the
; direct A/B of those answers, so a divergence names the register and the mask
; instead of the symptom.
;
; Rules cross-checked: IBM VGA Technical Reference (the CRTC and attribute index
; latches are 6 bits wide; reserved bits read back as zero; the colour/mono half
; of the port block is selected by miscellaneous output bit 0, and reading
; input status 1 resets the attribute flip-flop); QEMU hw/display/vga.c
; vga_ioport_read/vga_ioport_write for the write masks.
;
; Output: "<id><value as two hex digits>" per case, then CR LF. Values are what
; the card answered. The suite cell compares this against the committed copy of
; cemu's own answers (vga_readback_cemu.txt) -- a regression lock on the register
; file -- while vga_readback_qemu.txt holds the same sector's transcript under
; qemu-system-i386 and names the fields on which the two deliberately differ:
; the attribute-controller latch half of this file, because this card follows
; IBM VGA TR's chip-select line (IOCS16 gates only the CRTC pair) where QEMU's
; vga_ioport_invalid keeps the legacy whole-half mutual exclusion (AGENTS.md D17).
;
; Cases:
;  0  attribute index latch: reset the flip-flop (in 3DA), out 3C0=AA, in 3C0
;  1  the write that follows goes to an ATC register; 3C0 still shows the index
;  2  after in 3DA (flip-flop reset) out 3C0=22, in 3C0
;  3  CRTC index latch width: out 3D4=FF, in 3D4
;  4  CRTC register readback: out 3D4=06, in 3D5
;  5  misc written 67, read back at 3CC
;  6  the non-decoded half floats high: with misc bit 0 set, in 3BA
;  7  sequencer 0 written FF, read back at 3C5 (write mask)
;  8  graphics 0 written FF, read back at 3CF (write mask)
;  9  feature control written 11 through 3DA, then read back at 3CA
;  a  in 3C2 (input status 0)
;  b  DAC write index 55 written at 3C6, state read at 3C7
;  f    misc written 62, then in 3B4 (the mono half now decodes)
;  g    Y/N: does the retrace bit actually toggle for a polling guest?
;       (200 reads of 3DA, both phases must appear -- see the case for why the
;       judgement is printed rather than the count)
;
; Build: nasm -f bin -o vga_readback.bin vga_readback.asm
; Run:   cemu --machine x86 --isa x86 vga_readback.bin
;        qemu-system-i386 -drive file=vga_readback.bin,format=raw,if=ide,index=0 \
;            -boot c -display none -serial stdio -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit value 9 (QEMU status = (9<<1)|1 = 19) when the run completed.

  bits 16
  org 0x7c00

%define DBGEXIT 0xf4

%macro OUTB 2                  ; port, value
  mov dx, %1
  mov al, %2
  out dx, al
%endmacro

%macro INB 1                   ; port -> AL
  mov dx, %1
  in al, dx
%endmacro

%macro READ 2                  ; id character, port
  mov al, %1
  call putc
  INB %2
  call pethex
%endmacro

start:
  cli
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7c00
  call uart_init
  mov si, m_head
  call puts

  ; 0-2: the attribute controller's index latch and the flip-flop
  INB 0x3da                    ; reset the flip-flop
  OUTB 0x3c0, 0xaa
  READ '0', 0x3c0
  OUTB 0x3c0, 0x55             ; flip-flop was set -> an ATC register write
  READ '1', 0x3c0
  INB 0x3da                    ; reset again
  OUTB 0x3c0, 0x22
  READ '2', 0x3c0

  ; 3-4: the CRTC index latch width and a register readback
  OUTB 0x3d4, 0xff
  READ '3', 0x3d4
  OUTB 0x3d4, 0x06
  READ '4', 0x3d5

  ; 5-6: the miscellaneous output register and the decode gate
  OUTB 0x3c2, 0x67
  READ '5', 0x3cc
  READ '6', 0x3ba

  ; 7-8: the write masks
  OUTB 0x3c4, 0x00
  OUTB 0x3c5, 0xff
  READ '7', 0x3c5
  OUTB 0x3ce, 0x00
  OUTB 0x3cf, 0xff
  READ '8', 0x3cf

  ; 9-a: the feature control latch, and input status 0. 9 reads 3CA, not 3DA:
  ;      reading 3DA answers input status 1, whose bits depend on where the
  ;      raster is at that instant, so the byte it returns is not the card's
  ;      answer about the latch at all (IBM VGA TR: 3CA = feature control read,
  ;      3DA = input status 1 read / feature control write).
  OUTB 0x3da, 0x11
  READ '9', 0x3ca
  READ 'a', 0x3c2

  ; b: the DAC index
  OUTB 0x3c6, 0x55
  READ 'b', 0x3c7

  ; f: clear misc bit 0 (the mono half now decodes, so 3B4 answers)
  OUTB 0x3c2, 0x62
  READ 'f', 0x3b4

  ; g: is the retrace flag actually observable to a polling guest? Read 0x3DA
  ;    until both phases (bit 3 set and clear) have been seen, bounded at
  ;    ~1.3M reads. A fixed 200-sample count was the wrong instrument: the
  ;    vertical pulse is CR10..CR11's few scan lines (3 lines, ~0.7% of a
  ;    frame on the standard 80x25 table), so 200 samples can miss it while a
  ;    card that pulses correctly answers N. The transcript carries the
  ;    judgement, not the count -- a count depends on where the raster happens
  ;    to be when the guest polls, and a golden that differs between runs pins
  ;    nothing. Y = both phases seen, N = one phase missing.
  mov al, 'g'
  call putc
  OUTB 0x3c2, 0x67             ; back to the colour half
  xor bl, bl                   ; bit 3 seen set
  xor bh, bh                   ; bit 3 seen clear
  mov dx, 20                   ; 20 * 65535 reads spans many frame times
.outer:
  mov cx, 0xffff
.poll:
  INB 0x3da
  test al, 0x08
  jz .low
  mov bl, 1
  jmp .check
.low:
  mov bh, 1
.check:
  test bl, bl
  jz .next
  test bh, bh
  jnz .both
.next:
  loop .poll
  dec dx
  jnz .outer
.both:
  mov al, 'N'
  test bl, bl
  jz g_done
  test bh, bh
  jz g_done
  mov al, 'Y'
g_done:
  call putc

  call crlf
  mov al, 9
  out DBGEXIT, al
  hlt
  jmp $                       ; no debug-exit device: stop, never wrap

; ---- helpers ---------------------------------------------------------------

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

pethex:                       ; AL = byte -> two hex digits; AX, BX preserved
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
  OUTB 0x3fb, 0x80            ; LCR: divisor latch
  OUTB 0x3f8, 0x01            ; DLL = 1 (115200)
  OUTB 0x3f9, 0x00            ; DLH = 0
  OUTB 0x3fa, 0x00            ; IER = 0
  OUTB 0x3fb, 0x03            ; 8N1, DLAB = 0
  ret

m_head:   db "vga-rb ", 0

  times 510-($-$$) db 0
  dw 0xaa55
