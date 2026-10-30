; PIIX3/4 XBCS, the extended BIOS control byte at configuration offset 0x4E of
; the ISA bridge (not an acceptance test -- a probe that pins one chipset
; register).
;
; The register lives on 00:01.0. Its datasheet role is BIOSWE (bit 0) and BLE
; (bit 1): BIOS write enable and the lock that freezes it until reset. Nothing
; on this machine gates on it -- the E/F segment's write permission comes from
; the host bridge's PAM bits -- and QEMU's own bridge keeps it as plain storage
; with a reset value of 0x03, which is what this probe pins:
;
;   r0    the reset image, 0x03 (BIOSWE | BLE)
;   w2/w1/w0   a write of each bit pattern reads back unchanged, so the byte is
;         storage rather than write-one-to-set with a lock (the datasheet's
;         reading, registered in AGENTS.md §X D19)
;   wf    all eight bits store
;   pm    the same offset on 00:01.3 (the power-management function) is a
;         different register and reads 0 -- XBCS belongs to the ISA bridge
;
; Reference: PIIX4 datasheet register 4Eh; QEMU hw/isa/piix.c piix_reset
; (pci_conf[0x4e] = 0x03) and its pci_default_write_config path (no handler for
; the byte); hw/pci-host/i440fx.c (PAM is what gates the BIOS area's writes, and
; it has no BIOSWE term).
;
; Build: nasm -f elf32 -o xbcs.o xbcs.asm
;        ld.lld -m elf_i386 -T vbe_mode.ld -o xbcs.elf xbcs.o
; Run:   cemu --machine x86 --isa x86 xbcs.elf
;        qemu-system-i386 -machine pc -m 32 -kernel xbcs.elf -display none \
;            -no-reboot -device isa-debug-exit,iobase=0xf4,iosize=0x4 \
;            -serial file:xbcs_qemu.txt
; Exit: debug-exit 5 (status 11) once the transcript is out.

  bits 32

%define CFG_ADDR 0xcf8
%define CFG_DATA 0xcfc
; The dword that holds offset 0x4E for each function: register bits 7:2 select
; the dword, and the byte inside it is address 0x4E & 3 = 2, i.e. port 0xCFE.
%define CFG_ISA  0x8000084c      ; bus 0, device 1, function 0, offset 0x4C
%define CFG_PM   0x80000b4c      ; bus 0, device 1, function 3, offset 0x4C
%define DBGEXIT  0xf4

%macro SEL 1
  mov eax, %1
  mov dx, CFG_ADDR
  out dx, eax
%endmacro

; Read XBCS into AL.
%macro RDX 0
  mov dx, CFG_DATA + 2
  in al, dx
%endmacro

; Write AL into XBCS.
%macro WRX 0
  mov dx, CFG_DATA + 2
  out dx, al
%endmacro

%macro P 1
  mov esi, %1
  call puts
%endmacro

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

  SEL CFG_ISA
  P m_r0
  RDX
  call hex8

  ; Bit patterns, each read back where it was written.
  mov al, 0x02
  WRX
  P m_w2
  RDX
  call hex8
  mov al, 0x01
  WRX
  P m_w1
  RDX
  call hex8
  mov al, 0x00
  WRX
  P m_w0
  RDX
  call hex8
  mov al, 0xff
  WRX
  P m_wf
  RDX
  call hex8

  ; The same offset on the power-management function is not this register.
  SEL CFG_PM
  P m_pm
  RDX
  call hex8

  P m_ok
  P m_crlf
  mov dx, DBGEXIT
  mov al, 5
  out dx, al
  jmp $

; AL as two hex digits.
hex8:
  push eax
  mov ah, al
  shr al, 4
  call hexdigit
  mov al, ah
  call hexdigit
  pop eax
  ret

hexdigit:                       ; AL = 0..15
  and al, 0x0f
  cmp al, 10
  jb .digit
  add al, 'a' - 10
  jmp putc
.digit:
  add al, '0'
  jmp putc

puts:                           ; ESI = string, until NUL
  lodsb
  test al, al
  jz .done
  call putc
  jmp puts
.done:
  ret

putc:                           ; AL = character, waits for THRE
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

section .data
m_r0:    db "xbcs r0=", 0
m_w2:    db " w2=", 0
m_w1:    db " w1=", 0
m_w0:    db " w0=", 0
m_wf:    db " wf=", 0
m_pm:    db " pm=", 0
m_ok:    db " ok", 0
m_crlf:  db 13, 10, 0
