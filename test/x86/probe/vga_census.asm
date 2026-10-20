; The whole register file, as the card answers it at boot (not an acceptance test).
;
; SeaVGABIOS builds the "EGA/VGA information" block that int 10 AX=1000/1001
; returns by *reading the chip*: the graphics controller's misc/state register,
; the sequencer's clock mode, the attribute controller's mode, the CRTC's latch
; widths. A driver that then decides which display mode to ask for is deciding on
; those bytes. So a register file that answers differently is a different card,
; and the guest branches differently -- same firmware binary, same disk.
;
; This probe walks every readable register once and prints it, so a divergence
; names the register instead of the symptom. Output per register: "<group><index
; as two hex><value as two hex>", groups: C=CRTC(0x3D4/5), G=GCRT(0x3CE/F),
; S=sequencer(0x3C4/5), A=attribute(0x3C0 index + 0x3C1 read, flip-flop reset via
; 0x3DA before each).
;
; Values are whatever the card answered; the calibration is the same sector under
; qemu-system-i386 (vga_census_qemu.txt) and the two transcripts must match.
;
; Build: nasm -f bin -o vga_census.bin vga_census.asm
; Run:   cemu --machine x86 --isa x86 vga_census.bin
;        qemu-system-i386 -drive file=vga_census.bin,format=raw,if=ide,index=0 \
;            -boot c -display none -serial stdio -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit value 11 (QEMU status = (11<<1)|1 = 23).

  bits 16
  org 0x7c00

%define DBGEXIT 0xf4

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

  ; CRTC 0x00..0x11 (the colour half answers while misc bit 0 is set, which is
  ; how the BIOS leaves text mode)
  mov si, m_crtc
  mov bx, 0
  call walk

  ; Graphics controller 0x00..0x0F
  mov si, m_gfx
  mov bx, 0
  call walk

  ; Sequencer 0x00..0x07
  mov si, m_seq
  mov bx, 0
  call walk

  ; Attribute controller 0x00..0x11: index through 3C0 with the flip-flop
  ; reset first, data through 3C1.
  mov si, m_attr
  mov bx, 0
  call walk_attr

  ; p: the attribute pairing itself, with no BIOS history behind it: reset the
  ; flip-flop, write index 08 then value 5A, read 3C1 back. A card whose
  ; index/data pairing follows the writes answers 5A.
  mov dx, 0x3da
  in al, dx                 ; reset the attribute flip-flop
  mov dx, 0x3c0
  mov al, 0x08
  out dx, al
  mov dx, 0x3c0
  mov al, 0x5a
  out dx, al
  mov al, 'p'
  call putc
  mov dx, 0x3c1
  in al, dx
  call pethex

  ; q,r: the CRTC cursor-location pair, which shares CR14/CR15 with the start
  ; address: write 12/34 and read both back.
  mov dx, 0x3d4
  mov al, 0x0e
  out dx, al
  mov dx, 0x3d5
  mov al, 0x12
  out dx, al
  mov dx, 0x3d4
  mov al, 0x0f
  out dx, al
  mov dx, 0x3d5
  mov al, 0x34
  out dx, al
  mov al, 'q'
  call putc
  mov dx, 0x3d4
  mov al, 0x0e
  out dx, al
  mov dx, 0x3d5
  in al, dx
  call pethex
  mov al, 'r'
  call putc
  mov dx, 0x3d4
  mov al, 0x0f
  out dx, al
  mov dx, 0x3d5
  in al, dx
  call pethex

  ; s: the same pairing test with one read of 3C1 slipped in between the index
  ; write and the data write. If that read leaves the chip expecting data, the
  ; answer is the value (21); if the read re-arms "index expected", the 0x21
  ; byte becomes an index instead and the answer is AR[1] (01).
  mov dx, 0x3da
  in al, dx
  mov dx, 0x3c0
  mov al, 0x09
  out dx, al
  mov dx, 0x3c1
  in al, dx
  mov dx, 0x3c0
  mov al, 0x21
  out dx, al
  mov al, 's'
  call putc
  mov dx, 0x3c1
  in al, dx
  call pethex

  ; t: the control, no read in between -- must answer 25 in both.
  mov dx, 0x3da
  in al, dx
  mov dx, 0x3c0
  mov al, 0x0a
  out dx, al
  mov dx, 0x3c0
  mov al, 0x25
  out dx, al
  mov al, 't'
  call putc
  mov dx, 0x3c1
  in al, dx
  call pethex

  call crlf
  mov al, 11
  out DBGEXIT, al
  hlt
  jmp $

; ---- walk a register file: record = idxport(w) datport(w) count(b) group(b) ----
walk:
  push si
  mov dx, [si]
  xor ax, ax
  out dx, al                ; start at index 0
  mov bl, 0
.w1:
  mov dx, [si]
  mov al, bl
  out dx, al
  mov al, [si+5]
  call putc
  mov ax, bx
  call pethex
  mov dx, [si+2]
  in al, dx
  call pethex
  inc bl
  cmp bl, [si+4]
  jb .w1
  pop si
  ret

walk_attr:
  push si
  mov bl, 0
.a1:
  mov dx, 0x3da             ; reset the attribute flip-flop. DX form: 0x3DA does
  in al, dx                 ; not fit `in al, imm8`, and the immediate encoding
                            ; silently truncates to port 0xDA (nasm warns).
  mov dx, [si]
  mov al, bl
  out dx, al                ; index write
  mov al, [si+5]
  call putc
  mov ax, bx
  call pethex
  mov dx, [si+2]
  in al, dx                 ; data read from 3C1
  call pethex
  inc bl
  cmp bl, [si+4]
  jb .a1
  pop si
  ret

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

; idxport, datport, count, group
m_crtc:   dw 0x3d4, 0x3d5
          db 18, 'C'
m_gfx:    dw 0x3ce, 0x3cf
          db 16, 'G'
m_seq:    dw 0x3c4, 0x3c5
          db 8, 'S'
m_attr:   dw 0x3c0, 0x3c1
          db 18, 'A'
m_head:   db "census ", 0

  times 510-($-$$) db 0
  dw 0xaa55
