; AC alignment-check probe (self-contained multiboot ELF): at CPL 3 with
; CR0.AM=1 and EFLAGS.AC=1 an unaligned dword read raises #AC; the handler
; checks the saved CS RPL and EIP. Pass status 11.
;
; Build: nasm -f elf32 -o ac.o ac.asm
;        ld.lld -m elf_i386 -T ../pm/pm_smoke.ld -o ac.elf ac.o
; Run:   timeout 30 cemu --machine x86 --isa x86 ac.elf

bits 32

%define GDT     0x2000
%define GDTPTR  0x1f00
%define IDT     0x2100
%define IDTPTR  0x1f10
%define TSS     0x2400
%define RING0_TOP 0x4800
%define RING3_TOP 0x5000
%define SCRATCH   0x5100

section .multiboot
align 4
  dd 0x1BADB002
  dd 0x3
  dd -(0x1BADB002 + 0x3)

section .text
global _start
_start:
  cli
  ; GDT: null, ring0 code/data, ring3 code/data, 32-bit TSS at 0x2400.
  mov dword [GDT], 0
  mov dword [GDT + 4], 0
  mov dword [GDT + 8], 0x0000ffff
  mov dword [GDT + 12], 0x00cf9b00
  mov dword [GDT + 16], 0x0000ffff
  mov dword [GDT + 20], 0x00cf9300
  mov dword [GDT + 24], 0x0000ffff
  mov dword [GDT + 28], 0x00cffb00
  mov dword [GDT + 32], 0x0000ffff
  mov dword [GDT + 36], 0x00cff300
  mov dword [GDT + 40], 0x24000067
  mov dword [GDT + 44], 0x00008900
  mov word [GDTPTR], 47
  mov dword [GDTPTR + 2], GDT
  lgdt [GDTPTR]
  mov ax, 0x10
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  mov ss, ax
  mov esp, RING0_TOP
  jmp 0x08:.cs
.cs:

  ; TSS: ESP0/SS0 for the ring-3 -> ring-0 delivery, no I/O bitmap.
  mov dword [TSS], 0
  mov dword [TSS + 4], RING0_TOP
  mov word [TSS + 8], 0x10
  mov word [TSS + 0x66], 0x68
  mov ax, 0x28
  ltr ax

  ; IDT: #AC (17) is the test; unexpected faults report. Only the first 32
  ; vectors are zeroed: the full 2KiB range would overwrite the TSS at 0x2400.
  mov word [IDTPTR], 0xff
  mov dword [IDTPTR + 2], IDT
  xor eax, eax
  mov ecx, 0x40
  mov edi, IDT
  rep stosd
  mov eax, ac_handler
  call set_gate17
  mov eax, bad_handler
  call set_gate6
  mov eax, bad_handler
  call set_gate13
  mov eax, bad_handler
  call set_gate14
  lidt [IDTPTR]

  ; CR0.AM=1 enables the alignment check; EFLAGS.AC arrives in the IRET image.
  mov eax, cr0
  or eax, 0x40000
  mov cr0, eax
  mov dword [SCRATCH], 0x12345678

  push dword 0x23          ; SS = ring-3 data
  push dword RING3_TOP
  push dword 0x40002       ; EFLAGS: AC=1, IF=0, bit1
  push dword 0x1b          ; CS = ring-3 code, RPL 3
  push dword ring3_entry
  iretd

ring3_entry:
  mov ax, 0x23
  mov ds, ax
  mov es, ax
  ; Record the EFLAGS image the IRET delivered: AC must be set here.
  pushfd
  pop eax
  mov [SCRATCH + 4], eax
  test eax, 0x40000
  jz no_ac
  ; An aligned read is legal.
  mov eax, [SCRATCH]
  cmp eax, 0x12345678
  jne no_ac
  ; Unaligned dword: #AC must fire here.
ac_fault_site:
  mov eax, [SCRATCH + 1]
no_ac:
  ud2

ac_handler:
  ; [esp] error code, [esp+4] EIP, [esp+8] CS.
  mov eax, [esp + 8]
  and eax, 3
  cmp eax, 3
  jne bad_handler
  cmp dword [esp + 4], ac_fault_site
  jne bad_handler
  mov al, 5
  out 0xf4, al
  hlt

bad_handler:
  mov al, 0x0f
  out 0xf4, al
  hlt

set_gate17:
  mov [IDT + 17 * 8], ax
  mov word [IDT + 17 * 8 + 2], 0x08
  mov byte [IDT + 17 * 8 + 4], 0
  mov byte [IDT + 17 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + 17 * 8 + 6], ax
  ret
set_gate6:
  mov [IDT + 6 * 8], ax
  mov word [IDT + 6 * 8 + 2], 0x08
  mov byte [IDT + 6 * 8 + 4], 0
  mov byte [IDT + 6 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + 6 * 8 + 6], ax
  ret
set_gate13:
  mov [IDT + 13 * 8], ax
  mov word [IDT + 13 * 8 + 2], 0x08
  mov byte [IDT + 13 * 8 + 4], 0
  mov byte [IDT + 13 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + 13 * 8 + 6], ax
  ret
set_gate14:
  mov [IDT + 14 * 8], ax
  mov word [IDT + 14 * 8 + 2], 0x08
  mov byte [IDT + 14 * 8 + 4], 0
  mov byte [IDT + 14 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + 14 * 8 + 6], ax
  ret
