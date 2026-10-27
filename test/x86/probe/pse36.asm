; PSE-36 probe (self-contained multiboot ELF): a 4MB PDE at linear 4MiB
; whose bits 20:13 carry physical address bits 39:32. Reading it must reach
; physical 0x100000000 (open bus -> all ones), not fault; bit 21 stays
; reserved and faults. Pass status 11, failures status > 1.
;
; Build: nasm -f elf32 -o pse36.o pse36.asm
;        ld.lld -m elf_i386 -T ../pm/pm_smoke.ld -o pse36.elf pse36.o
; Run:   timeout 30 cemu --machine x86 --isa x86 pse36.elf

bits 32

%define GDT     0x2000
%define GDTPTR  0x1f00
%define IDT     0x3000
%define IDTPTR  0x2f00
%define PD      0x60000
%define B       0x80000

section .multiboot
align 4
  dd 0x1BADB002
  dd 0x3
  dd -(0x1BADB002 + 0x3)

section .text
global _start
_start:
  cli
  mov dword [GDT], 0
  mov dword [GDT + 4], 0
  mov dword [GDT + 8], 0x0000ffff
  mov dword [GDT + 12], 0x00cf9b00
  mov dword [GDT + 16], 0x0000ffff
  mov dword [GDT + 20], 0x00cf9300
  mov word [GDTPTR], 23
  mov dword [GDTPTR + 2], GDT
  lgdt [GDTPTR]
  mov ax, 0x10
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  mov ss, ax
  jmp 0x08:.cs
.cs:

  mov word [IDTPTR], 0x7ff
  mov dword [IDTPTR + 2], IDT
  xor eax, eax
  mov ecx, 0x400
  mov edi, IDT
  rep stosd
  mov eax, pf_handler
  mov [IDT + 14 * 8], ax
  mov word [IDT + 14 * 8 + 2], 0x08
  mov byte [IDT + 14 * 8 + 4], 0
  mov byte [IDT + 14 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + 14 * 8 + 6], ax
  lidt [IDTPTR]

  ; CPUID must advertise PSE-36 before the decode means anything.
  mov eax, 1
  cpuid
  test edx, 1 << 17
  jz fail1

  ; PD[0]: identity 4MB leaf so code, stack and IDT stay mapped.
  mov dword [PD], 0x00000083
  ; PD[1]: physical 0x100000000, 4MB leaf, P|RW. PDE[20:13] = 1 -> bit13.
  mov dword [PD + 4], 0x00002083
  ; PD[2]: bit 21 set (still reserved under PSE-36), P|RW|PS.
  mov dword [PD + 8], 0x00200083
  mov dword [PD + 12], 0
  mov eax, PD
  mov cr3, eax
  mov eax, cr4
  or eax, 0x10          ; PSE
  mov cr4, eax
  mov eax, cr0
  or eax, 0x80000001    ; PG | PE
  mov cr0, eax

  ; The PSE-36 page is accepted; the access lands on open bus.
  mov eax, [0x400000]
  cmp eax, 0xffffffff
  jne fail2

  ; PDE bit21 is reserved: this access must take #PF. The handler skips the
  ; 5-byte instruction and records the fault.
  mov byte [FAULTED], 0
  mov eax, [0x800000]
  cmp byte [FAULTED], 1
  jne fail3

  mov al, 5
  out 0xf4, al
  hlt

pf_handler:
  ; Stack: error, eip, cs, eflags.
  add dword [esp + 4], 5   ; mov eax, [disp32] is 5 bytes
  mov byte [FAULTED], 1
  add esp, 4               ; drop the error code
  iret

bad_handler:
  mov al, 0x0f
  out 0xf4, al
  hlt

fail1: mov al, 1
       jmp fail
fail2: mov al, 2
       jmp fail
fail3: mov al, 3
fail:
  out 0xf4, al
  hlt

section .bss
FAULTED: resb 1
