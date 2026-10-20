; The register file the firmware leaves behind (vga family core;
; test/x86/probe/vga_post.asm).
;
; Every other comparison sets the card up itself and compares what came of it.
; That misses the state a guest actually starts from: whatever SeaBIOS's POST and
; the video option ROM leave in the card. Both runs use the same two ROMs, and the
; video BIOS reads the card back while it works (to find the current mode), so a
; card that answers one bit differently ends up with a different register file
; here -- and every decision the guest makes later is made against a different
; machine.
;
; So this probe measures that state and nothing else: it boots as a boot sector,
; which means the firmware has already run, and it prints the card's registers
; exactly as they are, without touching any of them. (A boot sector rather than a
; multiboot image for exactly that reason: only the firmware boot path runs the
; video BIOS, so only it leaves the state worth measuring.)
;
; What it prints: SR8-SR1, GR9-GR1, AR20-AR1, CR25-CR1, and the miscellaneous
; output register with the status port's static bits. One line per register,
; self-labelled as <group><index>=<value>, so a transcript stays parseable even
; though SeaBIOS logs to the same COM1 and the two streams interleave.
;
; Build: nasm -f bin -o vga_post.img vga_post.asm, then pad that to 1474560
;        bytes (a 1.44MB floppy: the firmware refuses a size it does not know).
; Run:   cemu --machine x86 -bios <seabios>/out/bios.bin
;            -vgabios <seabios>/out/vgabios.bin -fda vga_post.img
;        qemu-system-i386 -machine pc -m 32 -fda vga_post.img -vga std
;            -bios <seabios>/out/bios.bin
;            -fw_cfg name=opt/isa-vga/rom,file=<seabios>/out/vgabios.bin
;            -display none -no-reboot -serial file:vga_post_qemu.txt
; Exit: debug-exit 5. The two transcripts must be identical.

  bits 16

%define DBGEXIT 0xf4

  org 0x7c00

start:
  cli                           ; a boot sector runs with interrupts off, and
  xor ax, ax                    ; leaving them on lets the timer interrupt
  mov ds, ax                    ; storm this short program
  mov es, ax
  mov ss, ax
  mov sp, 0x7c00
  call ser_init

  mov si, msg_sr
  call puts
  mov bh, 's'
  mov cx, 8
.sr:
  mov dx, 0x3c4                 ; puthex8 leaves dx on 0x3f8, so each round
  mov al, cl                    ; reloads the index port itself
  out dx, al
  inc dx                        ; the data port follows the index port
  in al, dx
  mov ah, al
  call putreg8
  loop .sr

  mov si, msg_gr
  call puts
  mov bh, 'g'
  mov cx, 9
.gr:
  mov dx, 0x3ce
  mov al, cl
  out dx, al
  inc dx
  in al, dx
  mov ah, al
  call putreg8
  loop .gr

  mov si, msg_ar
  call puts
  mov bh, 'a'
  mov cx, 20
.ar:
  mov al, cl
  push ax
  mov dx, 0x3da                 ; a 0x3da read puts the attribute flip-flop
  in al, dx                     ; back, so the write below reads as an index
  pop ax
  mov dx, 0x3c0
  out dx, al
  inc dx
  in al, dx
  mov ah, al
  call putreg8
  loop .ar

  mov si, msg_cr
  call puts
  mov bh, 'c'
  mov cx, 25
.cr:
  mov dx, 0x3d4
  mov al, cl
  out dx, al
  inc dx
  in al, dx
  mov ah, al
  call putreg8
  loop .cr

  mov si, msg_misc
  call puts
  mov dx, 0x3cc                 ; the miscellaneous output register ...
  in al, dx
  call puthex8
  mov dx, 0x3da
  in al, dx                     ; ... and the status port's static bits (the
  and al, 0xc0                  ; retrace bits move, so only 6-7 are printed)
  call puthex8
  call crlf

  mov si, msg_ok
  call puts
  call crlf
  mov al, 5
  jmp exit

; One register as <group><index>=<value>: bh = group letter, cl = index,
; ah = value. Self-labelling so an interleaved transcript stays parseable.
putreg8:
  mov al, bh
  call putchar
  mov al, 'r'
  call putchar
  mov al, cl
  call puthex8
  mov al, '='
  call putchar
  mov al, ah
  call puthex8
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
  mov ah, al                    ; the status poll below overwrites al
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

msg_sr:   db "sr:", 0
msg_gr:   db "gr:", 0
msg_ar:   db "ar:", 0
msg_cr:   db "cr:", 0
msg_misc: db "misc:", 0
msg_ok:   db "vga-post ok", 0

  times 510 - ($ - $$) db 0
  dw 0xaa55
