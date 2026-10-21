; VBE mode programming through DISPI (not an acceptance test -- a probe that
; pins one device's register and memory semantics).
;
; The Bochs DISPI mode set writes the geometry registers, turns on chain-4
; addressing and graphics mode, and moves the 64KB A0000 bank with the BANK
; register (seabios vgasrc/bochsvga.c + bochsvga.h; QEMU hw/display/vga.c's
; vbe_fixup_regs/vbe_update_vgaregs and bank_offset are the behavioural
; reference). The probe checks the registers a guest reads back, chain-4
; bytes through the aperture across a bank switch, and the packed 16/24/32bpp
; pixel bytes the linear framebuffer path will render.
;
; The render itself is not observable through a guest port; it is judged by
; CEMU_DEBUG=fbdump against QEMU's screendump (progress.md's VGA precedent).
;
; Build: nasm -f elf32 -o vbe_mode.o vbe_mode.asm
;        ld.lld -m elf_i386 -T vbe_mode.ld -o vbe_mode.elf vbe_mode.o
; Run:   cemu --machine x86 --isa x86 vbe_mode.elf
;        qemu-system-i386 -machine pc -m 32 -kernel vbe_mode.elf -vga std \
;            -display none -no-reboot -serial file:vbe_mode_qemu.txt
; Exit: debug-exit value 5 on pass (status = (5<<1)|1 = 11), 1 on fail.

  bits 32

%define DBGEXIT 0xf4
%define DISPI_INDEX 0x1ce
%define DISPI_DATA  0x1cf

section .multiboot
align 4
  dd 0x1BADB002
  dd 0x3
  dd -(0x1BADB002 + 0x3)

section .text
global _start

_start:
  cli
  call uart_init

  ; 1) 320x200x8, LFB enable. The DISPI registers read back the sanitized mode.
  mov byte [step_no], 1
  call dispi_disable
  mov eax, 1                 ; XRES
  mov ebx, 320
  call dispi_write
  mov eax, 2                 ; YRES
  mov ebx, 200
  call dispi_write
  mov eax, 3                 ; BPP
  mov ebx, 8
  call dispi_write
  mov eax, 4                 ; ENABLE
  mov ebx, 0x41              ; enabled | linear framebuffer
  call dispi_write
  mov eax, 1
  call dispi_read
  cmp ebx, 320
  jne fail
  mov eax, 2
  call dispi_read
  cmp ebx, 200
  jne fail
  mov eax, 3
  call dispi_read
  cmp ebx, 8
  jne fail

  ; 2) the mode set programs the VGA registers the ROM/guest reads back:
  ;    GR6 map 1, CR17 bits 0/1 set, chain-4 in SR4 and shift control 2 in GR5.
  mov byte [step_no], 0x21
  mov dx, 0x3ce
  mov al, 0x06
  out dx, al
  inc dx
  in al, dx
  mov bl, al
  and al, 0x0c
  cmp al, 0x04               ; GR6 map mode 1
  jne fail
  test bl, 0x01              ; graphics mode
  jz fail
  mov byte [step_no], 0x22
  mov dx, 0x3d4
  mov al, 0x17
  out dx, al
  inc dx
  in al, dx
  and al, 0x03
  cmp al, 0x03
  jne fail
  mov byte [step_no], 0x24
  mov dx, 0x3ce
  mov al, 0x05
  out dx, al
  inc dx
  in al, dx
  and al, 0x60
  cmp al, 0x40               ; shift control 2
  jne fail

  ; 3) chain-4 bytes through the aperture: three consecutive bytes land in
  ;    three different planes (the interleaved array's low two address bits).
  mov byte [step_no], 3
  mov byte [0xa0000], 0x12
  mov byte [0xa0001], 0x34
  mov byte [0xa0002], 0x56
  cmp byte [0xa0000], 0x12
  jne fail
  cmp byte [0xa0001], 0x34
  jne fail
  cmp byte [0xa0002], 0x56
  jne fail

  ; 4) BANK slides the 64KB window: bank 1's A0000 is a different byte, and
  ;    bank 0 still answers its own.
  mov byte [step_no], 4
  mov eax, 5                 ; BANK
  mov ebx, 1
  call dispi_write
  mov byte [0xa0000], 0x78
  mov eax, 5
  xor ebx, ebx
  call dispi_write
  cmp byte [0xa0000], 0x12
  jne fail
  mov eax, 5
  mov ebx, 1
  call dispi_write
  cmp byte [0xa0000], 0x78
  jne fail
  mov eax, 5
  xor ebx, ebx
  call dispi_write

  ; 5) 640x480x16: two bytes per pixel at A0000, chain-4 line pitch 1280.
  mov byte [step_no], 5
  call dispi_disable
  mov eax, 1
  mov ebx, 640
  call dispi_write
  mov eax, 2
  mov ebx, 480
  call dispi_write
  mov eax, 3
  mov ebx, 16
  call dispi_write
  mov eax, 4
  mov ebx, 0x41
  call dispi_write
  mov eax, 1
  call dispi_read
  cmp ebx, 640
  jne fail
  mov eax, 2
  call dispi_read
  cmp ebx, 480
  jne fail
  mov eax, 3
  call dispi_read
  cmp ebx, 16
  jne fail
  mov byte [0xa0000], 0x1f   ; blue in little-endian 5:6:5
  mov byte [0xa0001], 0x00
  mov byte [0xa0002], 0xe0   ; red at pixel 1
  mov byte [0xa0003], 0x03
  cmp byte [0xa0000], 0x1f
  jne fail
  cmp byte [0xa0003], 0x03
  jne fail

  ; 6) 800x600x24 and 1024x768x32 pixel bytes.
  mov byte [step_no], 6
  call dispi_disable
  mov eax, 1
  mov ebx, 800
  call dispi_write
  mov eax, 2
  mov ebx, 600
  call dispi_write
  mov eax, 3
  mov ebx, 24
  call dispi_write
  mov eax, 4
  mov ebx, 0x41
  call dispi_write
  cmp byte [0xa0000], 0x1f   ; a clear-on-enable mode starts at zero
  je fail
  mov byte [0xa0000], 0xaa
  mov byte [0xa0001], 0xbb
  mov byte [0xa0002], 0xcc
  cmp byte [0xa0000], 0xaa
  jne fail
  cmp byte [0xa0002], 0xcc
  jne fail
  call dispi_disable
  mov eax, 1
  mov ebx, 1024
  call dispi_write
  mov eax, 2
  mov ebx, 768
  call dispi_write
  mov eax, 3
  mov ebx, 32
  call dispi_write
  mov eax, 4
  mov ebx, 0x41
  call dispi_write
  mov byte [0xa0000], 0x11
  mov byte [0xa0001], 0x22
  mov byte [0xa0002], 0x33
  mov byte [0xa0003], 0x44
  cmp byte [0xa0003], 0x44
  jne fail

  mov esi, m_pass
  call puts
  ; Let the render interval elapse so a headless dump (fbdump/screendump) sees
  ; the mode the last step programmed, not the previous frame.
  mov ecx, 5000000
.delay:
  dec ecx
  jnz .delay
  call exit_ok
  jmp $

fail:
  mov esi, m_fail
  call puts
  mov al, [step_no]
  call puthex
  call exit_fail
  jmp $

exit_ok:
  mov dx, DBGEXIT
  mov eax, 5
  out dx, ax
  ret
exit_fail:
  mov dx, DBGEXIT
  mov eax, 1
  out dx, ax
  ret

; EAX = index, EBX = value
dispi_write:
  push eax
  push edx
  mov dx, DISPI_INDEX
  out dx, ax
  mov ax, bx
  mov dx, DISPI_DATA
  out dx, ax
  pop edx
  pop eax
  ret

; EAX = index -> EBX = value
dispi_read:
  push eax
  push edx
  mov dx, DISPI_INDEX
  out dx, ax
  mov dx, DISPI_DATA
  in ax, dx
  mov bx, ax
  pop edx
  pop eax
  ret

dispi_disable:
  push eax
  push ebx
  xor ebx, ebx
  mov eax, 4
  call dispi_write
  pop ebx
  pop eax
  ret

puthex:                       ; AL = byte, two hex digits
  push eax
  mov ah, al
  shr al, 4
  call .nib
  mov al, ah
  call .nib
  pop eax
  ret
.nib:
  and al, 0x0f
  cmp al, 10
  jb .dec
  add al, 'a' - 10
  jmp .out
.dec:
  add al, '0'
.out:
  call putc
  ret

puts:                         ; ESI = string, until NUL
  lodsb
  test al, al
  jz .done
  call putc
  jmp puts
.done:
  ret

putc:                         ; AL = character, waits for THRE
  push eax
  push esi
  push edx
  mov dx, 0x3fd
.wait:
  in al, dx
  test al, 0x20
  jz .wait
  pop edx
  pop esi
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

section .data
step_no:  db 0
m_pass:   db "vbe-mode ok", 13, 10, 0
m_fail:   db "vbe-mode FAIL step=", 0
