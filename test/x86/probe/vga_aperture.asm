; What the guest reads back out of the card's linear aperture (vga family core;
; test/x86/probe/vga_aperture.asm).
;
; The port file is settled: both models answer every register read and every
; register write the same way, access for access. The other half of the card's
; interface is the aperture -- the memory window at 0xA0000..0xBFFFF that a guest
; touches directly, with no port involved. If two cards answer the register file
; identically and a guest still behaves differently, the difference has to be
; here, and the only way to see it is to read the bytes back.
;
; So this probe writes a pattern through the aperture and reads it back: byte
; wide at an even and at an odd address (the sequencer's bank select is the low
; address bit, so a card that stores in one bank only fails here), then a word at
; a time, then the same eight kilobytes written as bytes and dumped -- so a
; mismatch between the write path and the read path cannot hide. Then a character
; cell, which is a byte pair plus an attribute byte: the text window's own layout,
; and the case where a word write gets split across banks wrongly.
;
; Run (the image is a 1.44MB floppy so the firmware accepts it):
;   cemu --machine x86 -bios <seabios>/out/bios.bin -vgabios <seabios>/out/vgabios.bin
;        -fda vga_aperture.img
;   qemu-system-i386 -machine pc -m 32 -bios <seabios>/out/bios.bin -fda
;        vga_aperture.img -vga std -fw_cfg name=opt/isa-vga/rom,file=<seabios>/out/vgabios.bin
;        -display none -no-reboot -serial file:vga_aperture_qemu.txt
; Exit: debug-exit 5. The two transcripts must match byte for byte.

  bits 16

%define DBGEXIT 0xf4
%define GFX     0xa0000            ; the graphics aperture
%define SCR     0xb8000            ; a character cell in the text window

  org 0x7c00

start:
  cli
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7c00
  call ser_init

  call gfx_mode                 ; decode the window for writing
  mov si, msg_oe
  call puts
  xor ax, ax
  mov es, ax
  mov di, GFX
  mov cx, 0x2000                ; 8KB of even addresses
  mov al, 0xa5
.even:
  mov [es:di], al
  add di, 2
  inc al                        ; the value changes, so a miss is visible
  loop .even
  mov di, GFX
  mov cx, 32
  call putbytes                 ; the even bytes, as stored
  call crlf
  mov di, GFX + 1
  mov cx, 32
  call putbytes                 ; and the odd ones, which select the other bank
  call crlf

  call gfx_mode
  mov si, msg_word
  call puts
  mov di, GFX
  mov cx, 0x1000                ; 0x1000 words = the same 8KB
  mov ax, 0x5aa5
.word:
  mov [es:di], ax
  add di, 2
  inc ax
  loop .word
  mov di, GFX
  mov cx, 32
  call putbytes                 ; a word write must read back as those two bytes
  call crlf

  mov si, msg_tfill
  call puts
  mov di, SCR
  mov cx, 32                    ; 16 character cells: character/attribute pairs
  xor al, al
.cell:
  mov [es:di], al               ; the character counts up ...
  mov ah, [es:di + 1]
  add ah, 0x40
  mov [es:di + 1], ah           ; ... and so does the attribute beside it
  inc al
  add di, 2
  loop .cell
  mov si, msg_dtext
  call puts
  mov di, SCR
  mov cx, 32
  call putbytes
  call crlf

  mov si, msg_ok
  call puts
  call crlf
  mov al, 5
  jmp exit

; Decode the aperture for writing: sequencer plane mask all four with no odd/even
; and no chain four, graphics controller in write mode 0 over the whole 256KB
; window, attribute controller mode control set for graphics rather than text.
gfx_mode:
  mov dx, 0x3c4                 ; sequencer 2: which planes a write touches
  mov al, 0x02
  out dx, al
  inc dx
  mov al, 0x0f
  out dx, al
  mov dx, 0x3c4                 ; sequencer 4: clear odd/even and chain four
  mov al, 0x04
  out dx, al
  inc dx
  xor al, al
  out dx, al
  mov dx, 0x3ce                 ; graphics 5: write mode 0, read mode 0
  mov al, 0x05
  out dx, al
  inc dx
  xor al, al
  out dx, al
  mov dx, 0x3ce                 ; graphics 6: the 256KB window at 0xA0000
  mov al, 0x06
  out dx, al
  inc dx
  mov al, 0x05
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
  mov al, 0x0f                  ; all four planes
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

msg_oe:    db "oe-even=", 0
msg_word:  db "word=", 0
msg_tfill: db "text-cell=", 0
msg_dtext: db "txt=", 0
msg_ok:    db "vga-aperture ok", 0

  times 510 - ($ - $$) db 0
  dw 0xaa55
