; Dump the standard-VGA Region-2 EDID blob (not an acceptance test -- a probe
; that reads one device's data). QEMU's stdvga maps a 384-byte EDID at BAR2
; offset 0 (hw/display/vga-pci.c pci_std_vga_mmio_region_init, edid default
; true); this probe programs BAR2 and prints the bytes so cemu's blob can be
; calibrated against the reference card.
;
; Build: nasm -f elf32 -o edid_dump.o edid_dump.asm
;        ld.lld -m elf_i386 -T vbe_mode.ld -o edid_dump.elf edid_dump.o
; Run:   qemu-system-i386 -machine pc -m 32 -kernel edid_dump.elf -vga std \
;            -display none -no-reboot -serial stdio
;        cemu --machine x86 --isa x86 edid_dump.elf

  bits 32

%define CFG_ADDR 0xcf8
%define CFG_DATA 0xcfc
%define BAR2_BASE 0xe0000000

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

  ; enable memory space, then program BAR2 (bus 0, device 2, function 0)
  mov eax, 0x80001004
  mov dx, CFG_ADDR
  out dx, eax
  mov dx, CFG_DATA
  in eax, dx
  or eax, 2
  mov dx, CFG_ADDR
  out dx, eax
  mov dx, CFG_DATA
  out dx, eax

  mov eax, 0x80001018
  mov dx, CFG_ADDR
  out dx, eax
  mov dx, CFG_DATA
  mov eax, BAR2_BASE
  out dx, eax

  ; print the two valid 128-byte EDID blocks, 16 per row
  xor ebx, ebx
.row:
  mov ecx, 16
  mov esi, BAR2_BASE
  add esi, ebx
.byte:
  movzx eax, byte [esi]
  call puthex
  inc esi
  loop .byte
  mov al, 13
  call putc
  mov al, 10
  call putc
  add ebx, 16
  cmp ebx, 256
  jb .row
  jmp $

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

putc:
  push eax
  push edx
  mov dx, 0x3fd
.wait:
  in al, dx
  test al, 0x20
  jz .wait
  pop edx
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
