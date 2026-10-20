; Mode 6's aperture: the one path the splash is painted through that nothing
; else here has covered (test/x86/probe/vga_cga.asm).
;
; vga_aperture programs a four-plane 256-colour shape at 0xA0000. Mode 6 is
; nothing like that: SeaBIOS's stdvgamodes gives it sequ_06 = {01,01,00,06},
; crtc_06 with R9=0xC1 and R17=0x8E, actl_06, and grdc_06 with GR6=0x0D. Read
; those three numbers together and the shape is: ONE plane (SR2=0x01), odd/even
; addressing ON (SR4 bit 2), and the window at 0xB8000 -- and CRTC R17 bit 0 clear,
; which is the CGA compatibility mode where the display address's bit 14 comes
; from the scan line's parity (IBM VGA TR 3.1a; QEMU's vga_draw_graphic
; recomputes the address as ((line & 1) << 14) | ... in that case).
;
; So a byte written at 0xB8000+A has no obvious home: the parity bit selects a
; bank, the plane mask selects a plane, and with only one plane enabled the odd
; addresses have nowhere to go. Windows 1.01 paints its whole splash here, and a
; guest that reads its own framebuffer back would see the difference -- which is
; the shape of a guest that decides the card is broken and falls back to text.
;
; So this probe programs mode 6 exactly as SeaBIOS's table has it, then:
;   - fills 0x1000 bytes through the window with a value that changes per byte,
;     one byte at a time;
;   - reads them back at the same addresses (does a write land where it was
;     addressed?), at the other parity (does the parity bit alias?), and as words
;     (does a word write split the way a byte pair does?);
;   - prints what came back. Two cards that disagree on any of it disagree about
;     a mode a real guest paints into.
;
; Run (the image is a 1.44MB floppy so the firmware accepts it):
;   cemu --machine x86 -bios <seabios>/out/bios.bin -vgabios <seabios>/out/vgabios.bin
;        -fda vga_cga.img
;   qemu-system-i386 -machine pc -m 32 -bios <seabios>/out/bios.bin -fda
;        vga_cga.img -vga std -fw_cfg name=opt/isa-vga/rom,file=<seabios>/out/vgabios.bin
;        -display none -no-reboot -serial file:vga_cga_qemu.txt
; Exit: debug-exit 5. The two transcripts must match byte for byte.

  bits 16

%define DBGEXIT 0xf4
%define WIN     0xb8000            ; mode 6's window

  org 0x7c00

start:
  cli
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7c00
  call ser_init
  call mode6

  ; Byte at a time over 4KB, the value counting up so a miss is visible.
  mov si, msg_fill
  call puts
  xor ax, ax
  mov es, ax
  mov di, WIN
  mov cx, 0x1000
  mov al, 0x11
.fill:
  mov [es:di], al
  inc di
  inc al
  loop .fill

  mov si, msg_same
  call puts
  mov di, WIN
  mov cx, 32
  call putbytes                  ; read back where it was written
  call crlf

  mov si, msg_par
  call puts
  mov di, WIN + 1
  mov cx, 32
  call putbytes                  ; the other parity: aliased, or its own bytes?
  call crlf

  ; Now the same window written a word at a time: a word is two bytes at two
  ; consecutive addresses, and with the parity bit in play the two halves may
  ; not land the way two byte writes would.
  call mode6
  mov si, msg_word
  call puts
  mov di, WIN
  mov cx, 0x0800
  mov ax, 0x1a2b
.word:
  mov [es:di], ax
  add di, 2
  inc ax
  loop .word
  mov di, WIN
  mov cx, 32
  call putbytes
  call crlf

  mov si, msg_ok
  call puts
  call crlf
  mov al, 5
  jmp exit

; Mode 6 as SeaBIOS's own table programs it: one plane, odd/even addressing on,
; the window at 0xB8000, and the attribute controller switched to graphics. The
; CRTC registers that shape the picture rather than the aperture (the scan-line
; table, the display end) are left as POST left them: this probe is about where
; a byte lands, not about the raster.
mode6:
  mov dx, 0x3c4                 ; sequencer 1: character height / shift
  mov al, 0x01
  out dx, al
  inc dx
  mov al, 0x01
  out dx, al
  mov dx, 0x3c4                 ; sequencer 2: which planes a write touches
  mov al, 0x02
  out dx, al
  inc dx
  mov al, 0x01                  ; plane 0 only
  out dx, al
  mov dx, 0x3c4                 ; sequencer 4: odd/even addressing, no chain 4
  mov al, 0x04
  out dx, al
  inc dx
  mov al, 0x06
  out dx, al
  mov dx, 0x3ce                 ; graphics 5: write mode 0, read mode 0
  mov al, 0x05
  out dx, al
  inc dx
  xor al, al
  out dx, al
  mov dx, 0x3ce                 ; graphics 6: the window at 0xB8000
  mov al, 0x06
  out dx, al
  inc dx
  mov al, 0x0d
  out dx, al
  mov dx, 0x3da                 ; a status read puts the attribute flip-flop back
  in al, dx
  mov dx, 0x3c0
  mov al, 0x10                  ; index 0x10: mode control
  out dx, al
  mov dx, 0x3c1
  mov al, 0x01                  ; graphics, not text
  out dx, al
  mov dx, 0x3da
  in al, dx
  mov dx, 0x3c0
  mov al, 0x12                  ; index 0x12: colour plane enable
  out dx, al
  mov dx, 0x3c1
  mov al, 0x01                  ; plane 0 only, as actl_06 has it
  out dx, al
  ret

putbytes:
  push cx
.next:
  mov al, [es:di]
  call puthex8
  inc di
  loop .next
  pop cx
  ret

ser_init:
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

puts:
.next:
  lodsb
  test al, al
  jz .done
  mov ah, al
  call putchar
  jmp .next
.done:
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
  call putchar
  ret

crlf:
  mov al, 13
  call putchar
  mov al, 10
  call putchar
  ret

putchar:
  push ax
  mov ah, al
  mov dx, 0x3fd
.wait:
  in al, dx
  test al, 0x20
  jz .wait
  mov dx, 0x3f8
  mov al, ah
  out dx, al
  pop ax
  ret

exit:
  mov dx, DBGEXIT
  out dx, al
  hlt
  jmp $

msg_fill: db "fill=", 0
msg_same: db "same=", 0
msg_par:  db "odd=", 0
msg_word: db "word=", 0
msg_ok:   db "vga-cga ok", 0

  times 510 - ($ - $$) db 0
  dw 0xaa55
