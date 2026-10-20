; DISPI, VBE's index/data pair at 0x1CE/0x1CF, and the bound on its register
; file (not an acceptance test -- a probe that pins one device's semantics).
;
; The option ROM's whole VBE gate is a handshake on this pair: it writes one of
; the six ids and reads it back (seabios/vgasrc/bochsvga.c:309-313), and the
; register it then reads to size the framebuffer is read-only and reports the
; card's own video memory in 64K units (bochsvga.c:347). The ports are word
; registers addressed with outw/inw (bochsvga.c:75-81), not two byte lanes, and
; the register file is exactly the eleven indices bochsvga.h:15-25 names.
;
; The bound is the point of this probe. VgaDevice holds `dispi[11]` directly in
; front of `PciDevice pci`, so an index that names no register turned a write to
; 0x1CF into a write into the card's PCI configuration space -- two `outw`s from
; any guest. Step 4 is that attack and step 5 reads the vendor/device pair back
; through 0xCF8/0xCFC to show nothing moved; step 6 proves the bound did not
; refuse the registers that do exist.
;
; Step 3 used to be the one answer this card did not share with a QEMU stdvga:
; it reported its own 256KB (4) where the reference card reports 16MB (256), and
; the option ROM sizes its mode table against that number, so cemu lost the VBE
; graphics modes SeaVGABIOS keeps on QEMU. The card now has 16MB (vga.h's
; kVgaVramSize). See test/x86/probe/vga_dispi_qemu.txt for the dual runs.
;
; Build: nasm -f bin -o vga_dispi.bin vga_dispi.asm
; Run:   cemu --machine x86 --isa x86 vga_dispi.bin
;        qemu-system-i386 -drive file=vga_dispi.bin,format=raw,if=ide,index=0 \
;            -boot c -vga std -display none -serial stdio -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit value 5 on pass (status = (5<<1)|1 = 11), 1 on fail.
; A failure names its step, because "the card answered wrong" is not diagnosable.

  bits 16
  org 0x7c00

%define DBGEXIT 0xf4

start:
  cli
  mov sp, 0x7c00
  call uart_init

  ; 1) the ROM's probe: write ID0, read it back
  mov byte [step_no], 1
  mov ax, 0x0000              ; VBE_DISPI_INDEX_ID
  mov dx, 0x1ce
  out dx, ax
  mov ax, 0xb0c0
  mov dx, 0x1cf
  out dx, ax
  in ax, dx
  cmp ax, 0xb0c0
  jne fail

  ; 2) an id outside the six changes nothing: the register keeps the id the card
  ;    already holds, and a later valid id takes it. (Measured on
  ;    qemu-system-i386 -vga std, which answers a write of 0x1234 by reading back
  ;    the 0xB0C0 it was already showing. A card that cleared its id on a stray
  ;    write would fail the ROM's own handshake right after passing it.)
  mov byte [step_no], 2
  mov ax, 0x1234
  out dx, ax
  in ax, dx
  cmp ax, 0xb0c0
  jne fail
  mov ax, 0xb0c5
  out dx, ax
  in ax, dx
  cmp ax, 0xb0c5
  jne fail

  ; 3) VIDEO_MEMORY_64K (index 0x0a) is read-only and answers the card's real
  ;    video memory in 64K units: 16MB here, as on a QEMU stdvga. A sizing write
  ;    changes nothing.
  mov byte [step_no], 3
  mov ax, 0x000a
  mov dx, 0x1ce
  out dx, ax
  mov ax, 0xffff
  mov dx, 0x1cf
  out dx, ax
  in ax, dx
  cmp ax, 0x0100
  jne fail

  ; 4) the attack: an index past the register file, in both encodings the old
  ;    code accepted (a plain 0x0f, and 0x8f whose bit 7 the old narrow-encoding
  ;    rule folded down to the same slot). Neither may move the data port.
  mov byte [step_no], 4
  mov ax, 0x000f
  mov dx, 0x1ce
  out dx, ax
  in ax, dx                   ; the index port answers the index it holds
  cmp ax, 0x000a
  jne fail
  mov ax, 0xffff
  mov dx, 0x1cf
  out dx, ax
  mov ax, 0x008f
  mov dx, 0x1ce
  out dx, ax
  in ax, dx
  cmp ax, 0x000a
  jne fail
  mov ax, 0xffff
  mov dx, 0x1cf
  out dx, ax                  ; the write that used to land in config space

  ; 5) 00:02.0 is still 1234:1111 (vga.h kPciVendorStdVga / kPciDeviceStdVga)
  mov byte [step_no], 5
  mov eax, 0x80001000         ; enable, bus 0, device 2, register 0
  mov dx, 0xcf8
  out dx, eax
  mov dx, 0xcfc
  in eax, dx
  cmp eax, 0x11111234
  jne fail

  ; 6) a register that does exist still stores: Y_OFFSET (index 0x09)
  mov byte [step_no], 6
  mov ax, 0x0009
  mov dx, 0x1ce
  out dx, ax
  mov ax, 0x1234
  mov dx, 0x1cf
  out dx, ax
  in ax, dx
  cmp ax, 0x1234
  jne fail

  mov si, m_pass
  call puts
  mov dx, DBGEXIT
  mov ax, 5
  out dx, ax
  jmp $

fail:
  mov si, m_fail
  call puts
  mov al, [step_no]
  add al, '0'
  call putc
  mov dx, DBGEXIT
  mov ax, 1
  out dx, ax
  jmp $

puts:                         ; SI = string, until NUL
  lodsb
  test al, al
  jz .done
  call putc
  jmp puts
.done:
  ret

putc:                         ; AL = character, waits for THRE
  push ax
  push si
  mov dx, 0x3fd
.wait:
  in al, dx
  test al, 0x20
  jz .wait
  pop si
  pop ax
  mov dx, 0x3f8
  out dx, al
  ret

uart_init:
  mov dx, 0x3fb               ; LCR: divisor latch
  mov al, 0x80
  out dx, al
  mov dx, 0x3f8               ; DLL = 1 (115200 baud)
  mov al, 0x01
  out dx, al
  mov dx, 0x3f9               ; DLH = 0
  xor al, al
  out dx, al
  mov dx, 0x3fb               ; 8N1, DLAB = 0
  mov al, 0x03
  out dx, al
  mov dx, 0x3f9               ; IER = 0
  xor al, al
  out dx, al
  ret

step_no:  db 0
m_pass:   db "vga-dispi ok", 13, 10, 0
m_fail:   db "vga-dispi FAIL step=", 0

  times 510-($-$$) db 0
  dw 0xaa55
