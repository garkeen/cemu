; The graphics picture the register file describes (vga family core, guest-driven).
;
; The card's graphics path had no automated coverage once the BIOS-booting `vga`
; suite cell was deleted: vga_core.asm pins the register file, not the picture.
; This probe fills that hole the sanctioned way -- it programs the card itself
; (no firmware, no int10, no disk image), paints through the aperture the way a
; real driver does, reports what the card reads back, and leaves the rendered
; framebuffer to be taken host-side with CEMU_DEBUG=fbdump= (cemu) or the
; monitor's `screendump` (QEMU). Both dumps are byte-comparable P6 PPMs.
;
; The mode is the one Windows 1.01's EGA display driver asks for, taken from the
; video BIOS's own standard-VGA table (SeaVGABIOS vgasrc/stdvgamodes.c: sequ_0e,
; crtc_0f, actl_10, grdc_0d, miscreg 0xa3 -- 640x350, four planes, 4bpp planar).
; R13 (the line offset) is the value the BIOS programs at run time from the
; mode's line length (stdvga_set_linelength: 320 bytes -> 40).
;
; How a driver paints this mode, and what it therefore pins: the four plane
; bytes of one pixel byte are one dword of the interleaved planes (QEMU's
; vga_draw_line4 reads exactly one dword per pixel byte and takes a lane per
; plane), so the guest writes *one* byte per pixel byte and lets write mode 2
; expand its low nibble across the planes -- not four bytes at four addresses.
; The ruler below marks that distinction, because getting it wrong is exactly
; the kind of bug that leaves the card's picture unrecognisable:
;   * left edge (pixel byte 0) and right edge (pixel byte 79) lit on every row
;   * a row barcode in pixels 8-15 (row & 1) and 16-23 ((row >> 1) & 1), so the
;     dump says which VRAM row each displayed row came from and a one-row shift
;     or a doubled row cannot hide
;   * row 100 filled with colour 15, row 300 with colour 3
; The DAC is programmed by the probe (entry 15 white, entry 3 blue, the rest
; black), so the dump's colours mean the same thing on every model.
;
; Build: nasm -f elf32 -o vga_paint.o vga_paint.asm
;        ld.lld -m elf_i386 -T vga_paint.ld -o vga_paint.elf vga_paint.o
; Run:   cemu --machine x86 --isa x86 vga_paint.elf
;          (CEMU_DEBUG=fbdump=vga_paint.ppm for the picture)
;        qemu-system-i386 -machine pc -m 32 -kernel vga_paint.elf -vga std
;            -display none -no-reboot -serial file:vga_paint_qemu.txt
;          then the monitor's `screendump vga_paint_qemu.ppm`
; Exit: debug-exit 5. The verdict is the serial line plus the framebuffer.

  bits 32

%define DBGEXIT 0xf4

; ---- port map (IBM VGA Technical Reference ch. 4) ----------------------------
%define P_SEQ_INDEX  0x3c4
%define P_SEQ_DATA   0x3c5
%define P_GFX_INDEX  0x3ce
%define P_GFX_DATA   0x3cf
%define P_ATTR       0x3c0
%define P_ATTR_DATA  0x3c1
%define P_DAC_WIDX   0x3c8
%define P_DAC_DATA   0x3c9
%define P_CRTC_INDEX 0x3d4
%define P_CRTC_DATA  0x3d5
%define P_STATUS1    0x3da
%define P_MISC_W     0x3c2

; The aperture, identity mapped (multiboot hands us flat 4 GiB segments with
; paging off, so linear == physical).
%define APERTURE     0xa0000

; The mode 0x10 geometry: 640 pixels of eight dots, a 350-line display, and 80
; pixel bytes per row (the four plane bytes of one pixel byte are one dword, so
; a row is 80 plane offsets = 320 bytes).
%define PIX_BYTES    80
%define ROWS         350

section .multiboot
align 4
  dd 0x1BADB002                 ; magic
  dd 0x3                        ; PAGE_ALIGN | MEMORY_INFO
  dd -(0x1BADB002 + 0x3)        ; checksum

section .text
global _start

_start:
  cli
  call uart_init

  ; The 0x3B0 half of the port block is mutually exclusive with the 0x3D0 one;
  ; the colour decode is what the mode's CRTC lives behind.
  mov dx, P_MISC_W
  mov al, 0x01
  out dx, al

  call dac            ; the palette first: the dump's colours depend on it
  call attr_table
  call seq_table
  call gfx_table
  call crtc_table

  ; The miscellaneous output register the mode's table carries (0xa3: colour
  ; decode, RAM on, 28.322 MHz dot clock, both syncs positive).
  mov dx, P_MISC_W
  mov al, 0xa3
  out dx, al

  call paint
  call report         ; what the card reads back, and what the paint left in it
  call settle         ; let the host render the picture it now describes

  mov esi, m_ok
  call puts
  call crlf
  mov al, 5
  jmp exit

; ---- the DAC: entry 15 white, entry 3 blue, everything else black -------------
; Programming it here rather than trusting the reset palette is what makes the
; dumped picture comparable across models: the card comes up with the IBM reset
; palette, whose entry 0 is not black.
dac:
  mov dx, P_DAC_WIDX
  xor al, al
  out dx, al
  mov esi, pal
  mov ecx, 48                  ; 16 entries of three 6-bit components
.dac_byte:
  lodsb
  mov dx, P_DAC_DATA
  out dx, al
  dec ecx
  jnz .dac_byte
  ret

; ---- attribute controller: AR0-AR19 from the mode's table --------------------
; The attribute registers are addressed by number: the palette is 0x00-0x0f and
; the four control registers are 0x10 (mode control: graphics), 0x11 (overscan),
; 0x12 (colour plane enable: four planes) and 0x13 (panning).
attr_table:
  xor ecx, ecx
.attr:
  mov al, cl
  lea esi, [actl + ecx]
  mov dl, [esi]
  call attr_write
  inc ecx
  cmp ecx, 20
  jb .attr
  ret

attr_write:                      ; al = index, dl = value
  push edx
  push eax
  mov dx, P_STATUS1              ; a status read puts the flip-flop back
  in al, dx
  pop eax
  mov dx, P_ATTR
  out dx, al                    ; index
  pop edx
  push edx
  mov al, dl
  mov dx, P_ATTR
  out dx, al                    ; data, same port (the flip-flop toggles)
  pop edx
  ret

; ---- sequencer: SR1-SR4 from the mode's table --------------------------------
; sequ_regs[0..3] are SR1..SR4 (stdvga_set_mode writes them as regs[1..4]).
seq_table:
  xor ecx, ecx
.seq:
  lea esi, [sequ + ecx]
  mov bl, [esi]
  mov al, cl
  inc al                      ; SR1..SR4
  mov dx, P_SEQ_INDEX
  out dx, al
  mov al, bl
  mov dx, P_SEQ_DATA
  out dx, al
  inc ecx
  cmp ecx, 4
  jb .seq
  ret

; ---- graphics controller: GR0-GR8 from the mode's table ----------------------
; GR5 gets write mode 2 instead of the table's mode 0 (see the header).
gfx_table:
  xor ecx, ecx
.gfx:
  mov al, cl
  mov dx, P_GFX_INDEX
  out dx, al
  lea esi, [grdc + ecx]
  mov al, [esi]
  cmp cl, 5                     ; GR5: write mode 2, no rotate
  jne .gfx_write
  mov al, 0x02
.gfx_write:
  mov dx, P_GFX_DATA
  out dx, al
  inc ecx
  cmp ecx, 9
  jb .gfx
  ret

; ---- CRTC: R0-R24 from the mode's table, plus the run-time line offset -------
; R11's bit 7 protects R0-R7 (IBM VGA TR 3.1a), so it is cleared first.
crtc_table:
  mov dx, P_CRTC_INDEX
  mov al, 0x11
  out dx, al
  xor al, al
  mov dx, P_CRTC_DATA
  out dx, al

  xor ecx, ecx
.crtc:
  mov al, cl
  mov dx, P_CRTC_INDEX
  out dx, al
  lea esi, [crtc + ecx]
  mov al, [esi]
  mov dx, P_CRTC_DATA
  out dx, al
  inc ecx
  cmp ecx, 25
  jb .crtc

  ; R13: the mode's line is 320 bytes, which is the 40 the BIOS programs here
  ; (stdvga_set_linelength divides the line length by eight, so the register
  ; counts eight bytes per step whatever the plane count).
  mov dx, P_CRTC_INDEX
  mov al, 0x13
  out dx, al
  mov al, 40
  mov dx, P_CRTC_DATA
  out dx, al
  ret

; ---- the paint: one byte per pixel byte, write mode 2 expands it --------------
; Per row r:
;   pixel byte 0    colour 15                  -> pixels 0-7 lit on every row
;   pixel byte 1    colour 15 when r & 1       -> pixels 8-15 on odd rows
;   pixel byte 2    colour 15 when (r >> 1) & 1-> pixels 16-23 on those rows
;   pixel byte 79   colour 15                  -> pixels 632-639 on every row
;   row 100         every byte 15             -> the whole row lit
;   row 300         every byte 3              -> the whole row in entry 3
paint:
  mov esi, APERTURE
  xor ebx, ebx                  ; ebx = row
.p_row:
  mov byte [esi + 0], 15        ; the left edge
  mov byte [esi + 1], 0         ; the row's low bit
  test bl, 1
  jz .p_n1_done
  mov byte [esi + 1], 15
.p_n1_done:
  mov byte [esi + 2], 0         ; the row's second bit
  mov ecx, ebx
  shr ecx, 1
  test cl, 1
  jz .p_n2_done
  mov byte [esi + 2], 15
.p_n2_done:
  mov byte [esi + 79], 15       ; the right edge

  cmp ebx, 100
  jne .p_not100
  mov edi, esi
  mov ecx, PIX_BYTES
  mov al, 15
.p_fill100:
  mov [edi], al
  inc edi
  dec ecx
  jnz .p_fill100
.p_not100:

  cmp ebx, 300
  jne .p_not300
  mov edi, esi
  mov ecx, PIX_BYTES
  mov al, 3
.p_fill300:
  mov [edi], al
  inc edi
  dec ecx
  jnz .p_fill300
.p_not300:

  add esi, PIX_BYTES            ; the plane offsets of one row (80)
  inc ebx
  cmp ebx, ROWS
  jb .p_row
  ret

; ---- what the card reads back, and what the paint left in it -----------------
; The framebuffer is the verdict, but a blank or scrambled dump cannot say
; whether the registers took or the paint landed, so the probe reports both: the
; register file the card now holds, and the four plane bytes of two pixel bytes
; (row 0's first and row 100's first). Everything is guest-visible, so the line
; is the same on every model that behaves the same way.
report:
  mov esi, m_crtc
  call puts
  mov dx, P_CRTC_INDEX
  call crtc_get
  call puthex8
  mov esi, m_comma
  call puts
  mov dx, P_CRTC_INDEX
  mov al, 0x01
  call crtc_get
  call puthex8
  mov esi, m_comma
  call puts
  mov dx, P_CRTC_INDEX
  mov al, 0x09
  call crtc_get
  call puthex8
  mov esi, m_comma
  call puts
  mov dx, P_CRTC_INDEX
  mov al, 0x13
  call crtc_get
  call puthex8
  mov esi, m_crtc2
  call puts
  mov dx, P_CRTC_INDEX
  mov al, 0x12
  call crtc_get
  call puthex8
  mov esi, m_comma
  call puts
  mov dx, P_CRTC_INDEX
  mov al, 0x07
  call crtc_get
  call puthex8
  mov esi, m_attr
  call puts
  mov al, 0x10
  call attr_get
  call puthex8
  mov esi, m_comma
  call puts
  mov al, 0x12
  call attr_get
  call puthex8
  mov esi, m_sr
  call puts
  mov dx, P_SEQ_INDEX
  mov al, 0x02
  call seq_get
  call puthex8
  mov esi, m_comma
  call puts
  mov dx, P_SEQ_INDEX
  mov al, 0x04
  call seq_get
  call puthex8
  mov esi, m_gr
  call puts
  mov dx, P_GFX_INDEX
  mov al, 0x05
  call gfx_get
  call puthex8
  mov esi, m_planes0
  call puts
  mov esi, APERTURE                ; pixel byte 0 of row 0
  call read_four_planes
  mov esi, m_plane_row
  call puts
  mov esi, APERTURE + PIX_BYTES    ; pixel byte 0 of row 1
  call read_plane0
  mov esi, m_comma
  call puts
  mov esi, APERTURE + 79           ; the right edge of row 0
  call read_plane0
  mov esi, m_comma
  call puts
  mov esi, APERTURE + 2 * PIX_BYTES + 79
  call read_plane0
  mov esi, m_plane_100
  call puts
  mov esi, APERTURE + 100 * PIX_BYTES
  call read_plane0
  mov esi, m_comma
  call puts
  mov esi, APERTURE + 100 * PIX_BYTES + 79
  call read_plane0
  mov esi, m_comma
  call puts
  mov esi, APERTURE + 300 * PIX_BYTES
  call read_plane0
  mov esi, m_crlf
  call puts
  ret

; Plane 0's byte at the aperture address esi, with GR4 set to plane 0.
read_plane0:
  push esi
  mov dx, P_GFX_INDEX
  mov al, 4
  out dx, al
  xor al, al
  mov dx, P_GFX_DATA
  out dx, al
  movzx eax, byte [esi]
  call puthex8
  pop esi
  ret

; The four plane bytes of the pixel byte at esi, read one plane at a time: GR4's
; read map select names the plane and a byte read returns it (QEMU's
; vga_mem_readb, read mode 0). All four reading f means write mode 2 expanded
; the colour across the planes.
read_four_planes:
  push esi
  mov byte [plane], 4
.rfp:
  dec byte [plane]                  ; planes 3, 2, 1, 0
  mov dx, P_GFX_INDEX
  mov al, 4                       ; GR4: read map select
  out dx, al
  mov al, [plane]
  mov dx, P_GFX_DATA
  out dx, al
  movzx eax, byte [esi]           ; the aperture byte, plane GR4 names
  call puthex8
  cmp byte [plane], 0
  jne .rfp
  pop esi
  ret

crtc_get:                        ; al = index -> al = value
  mov dx, P_CRTC_INDEX
  out dx, al
  mov dx, P_CRTC_DATA
  in al, dx
  ret

attr_get:                        ; al = index -> al = value (the 0x3C1 path)
  push eax
  mov dx, P_STATUS1
  in al, dx
  pop eax
  mov dx, P_ATTR
  out dx, al
  mov dx, P_ATTR_DATA
  in al, dx
  ret

seq_get:                         ; al = index -> al = value
  mov dx, P_SEQ_INDEX
  out dx, al
  mov dx, P_SEQ_DATA
  in al, dx
  ret

gfx_get:                         ; al = index -> al = value
  mov dx, P_GFX_INDEX
  out dx, al
  mov dx, P_GFX_DATA
  in al, dx
  ret

; ---- give the host time to render the picture -------------------------------
; The card re-renders on a timer (VgaPoll's interval, AGENTS.md §十's fbdump
; needs the frame that is on screen when the session ends), so this is a plain
; delay: a few million instructions is far longer than one render interval.
settle:
  mov ecx, 0x300000
.settle:
  dec ecx
  jnz .settle
  ret

; ---- serial output -----------------------------------------------------------
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

puthex8:
  push eax
  shr al, 4
  call .nib
  pop eax
  and al, 0x0f
.nib:
  cmp al, 10
  jb .digit
  add al, 'a' - 10
  jmp .emit
.digit:
  add al, '0'
.emit:
  push eax
  call putc
  pop eax
  ret

putc:
  push eax
  mov dx, 0x3fd
.wait:
  in al, dx
  test al, 0x20
  jz .wait
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

exit:
  mov dx, DBGEXIT
  out dx, al
  hlt
  jmp $

; ---- data --------------------------------------------------------------------
; SeaVGABIOS vgasrc/stdvgamodes.c, mode 0x10: sequ_0e, grdc_0d, actl_10,
; crtc_0f. The CRTC table's R13 is zero in the BIOS's table; the run-time line
; length sets it (see crtc_table).
sequ:   db 0x01, 0x0f, 0x00, 0x06
grdc:   db 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0f, 0xff
; actl_10: AR0-AR15 the palette, AR16 mode control (graphics), AR17 overscan,
; AR18 colour plane enable (four planes), AR19 panning.
actl:   db 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07
actl2:  db 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f
actl3:  db 0x01, 0x00, 0x0f, 0x00
crtc:   db 0x5f, 0x4f, 0x50, 0x82, 0x54, 0x80, 0xbf, 0x1f
crtc2:  db 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
crtc3:  db 0x83, 0x85, 0x5d, 0x28, 0x0f, 0x63, 0xba, 0xe3
crtc4:  db 0xff
m_ok:   db "vga-paint ok", 13, 10, 0
m_crtc: db "cr00,01,09,13=", 0
m_crtc2: db ",12=", 0
m_attr: db " ar10,12=", 0
m_sr:   db " sr2,4=", 0
m_gr:   db " gr5=", 0
m_comma: db ",", 0
m_planes0: db " p0=", 0
m_plane_row: db " r1p0=", 0
m_plane_100: db " r100p0=", 0
m_crlf: db 13, 10, 0
plane:  db 0
; The 16-entry DAC image: entry 3 blue, entry 15 white, the rest black.
pal:    db 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x3f
        db 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00
        db 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00
        db 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00, 0x3f,0x3f,0x3f