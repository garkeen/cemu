; I/O APIC NMI delivery (not an acceptance test -- a probe for one delivery
; mode).
;
; The probe builds an IDT with a vector-2 gate, programs I/O APIC pin 0 (the
; PIT's IRQ0 wire) as NMI delivery to the boot processor, masks the 8259 so
; only the I/O APIC can deliver, starts the PIT at 100 Hz, clears IF and spins
; on a flag the NMI handler increments. NMI is not gated by IF, so the flag
; moving proves the chipset's NMI delivery mode reached the processor's NMI
; pin (82093AA §3.2.4; SDM vol.3 6.7).
;
; Build: nasm -f elf32 -o nmi_ioapic.o nmi_ioapic.asm
;        ld.lld -m elf_i386 -T vbe_mode.ld -o nmi_ioapic.elf nmi_ioapic.o
; Run:   cemu --machine x86 --isa x86 nmi_ioapic.elf
;        qemu-system-i386 -machine pc -m 32 -kernel nmi_ioapic.elf -vga std \
;            -display none -no-reboot -serial stdio
;        (QEMU has the isa-debug-exit device only when asked for; without it
;         the verdict is the serial line)
; Exit: debug-exit 5 on cemu; QEMU prints nmi-ok and then spins.

  bits 32

%define DBGEXIT   0xf4
%define IOAPIC    0xfec00000
%define LAPIC     0xfee00000
%define FLAG      0x3000

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

  ; A real GDT: the IDT gate reloads CS=0x08 through it, and multiboot's
  ; synthetic selectors are not a table a gate return can name.
  mov edi, 0x8800
  mov dword [edi + 0], 0
  mov dword [edi + 4], 0
  mov dword [edi + 8], 0x0000ffff
  mov dword [edi + 12], 0x00cf9a00
  mov dword [edi + 16], 0x0000ffff
  mov dword [edi + 20], 0x00cf9200
  mov word [gdtr], 23
  mov dword [gdtr + 2], 0x8800
  lgdt [gdtr]
  mov ax, 0x10
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  mov ss, ax
  jmp 0x08:.gdt_loaded
.gdt_loaded:

  ; IDT at 0x8000: vector 2 -> nmi_handler, 32-bit interrupt gate, CS=0x08.
  mov edi, 0x8000
  mov ecx, 512
  xor eax, eax
  rep stosd
  mov eax, nmi_handler
  mov word [0x8000 + 2 * 8 + 0], ax
  shr eax, 16
  mov word [0x8000 + 2 * 8 + 6], ax
  mov word [0x8000 + 2 * 8 + 2], 0x08
  mov byte [0x8000 + 2 * 8 + 4], 0
  mov byte [0x8000 + 2 * 8 + 5], 0x8e
  mov word [idtr], 0x7ff
  mov dword [idtr + 2], 0x8000
  lidt [idtr]

  ; LAPIC software-enable so a model that checks SVR still accepts the NMI.
  mov ecx, 0x1b
  mov eax, LAPIC | 0x800
  xor edx, edx
  wrmsr
  mov dword [LAPIC + 0xf0], 0x1ff

  ; All 8259 lines masked: the PIT's request must arrive through the I/O APIC.
  mov al, 0xff
  out 0x21, al
  out 0xa1, al

  ; I/O APIC pin 0 entry: NMI (delivery 4), physical destination 0, unmasked.
  mov dword [IOAPIC + 0], 0x10           ; IOREGSEL = redirection table base
  mov dword [IOAPIC + 0x10], 0x00000400  ; vector 0 | NMI delivery | mask=0
  mov dword [IOAPIC + 0], 0x11
  mov dword [IOAPIC + 0x10], 0x00000000  ; destination APIC id 0

  mov dword [FLAG], 0
  ; PIT channel 0: mode 3, divisor 11932 (~100 Hz).
  mov al, 0x36
  out 0x43, al
  mov ax, 11932
  out 0x40, al
  mov al, ah
  out 0x40, al

  ; Spin with interrupts disabled: only the NMI pin can move the flag.
  mov ecx, 0x2000000
.spin:
  mov eax, [FLAG]
  test eax, eax
  jnz .ok
  dec ecx
  jnz .spin
  jmp fail
.ok:
  mov esi, m_ok
  call puts
  mov dx, DBGEXIT
  mov eax, 5
  out dx, ax
  jmp $

fail:
  mov esi, m_fail
  call puts
  mov dx, DBGEXIT
  mov eax, 1
  out dx, ax
  jmp $

nmi_handler:
  push eax
  inc dword [FLAG]
  pop eax
  iret

puts:
  lodsb
  test al, al
  jz .done
  call putc
  jmp puts
.done:
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

section .data
gdtr:     dw 0, 0, 0, 0
idtr:     dw 0, 0, 0, 0
m_ok:     db "nmi-ok", 13, 10, 0
m_fail:   db "nmi-FAIL", 13, 10, 0
