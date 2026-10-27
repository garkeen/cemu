; Memory/TLB benchmark (self-contained multiboot ELF, no devices/BIOS).
; Identity-maps 0-8 MiB with 4 KiB pages, enables paging, then runs two
; pointer-chasing-free phases:
;   phase 1: 256 pages  (fits the 512-set x 2-way read/write TLB)
;   phase 2: 1024 pages (thrashes it)
; Both phases read + RMW one dword per page in a tight loop, so the measured
; time is dominated by fetch + translation + data access. Exit through the
; QEMU isa-debug-exit port 0xF4 with payload 5 (status = 11).
;
; Build: nasm -f elf32 -o membench.o membench.asm
;        ld.lld -m elf_i386 -T ../pm/pm_smoke.ld -o membench.elf membench.o
; Run:   timeout 60 cemu --machine x86 --isa x86 membench.elf

bits 32

%define PD      0x60000
%define PT0     0x61000
%define PT1     0x62000
%define BUF     0x100000
%define NPHASE1 2000000
%define NPHASE2 1000000
%define IDT     0x3000
%define IDTPTR  0x2f00
%define GDT     0x2000
%define GDTPTR  0x1f00
%define FAULT   0x9000

section .multiboot
align 4
  dd 0x1BADB002
  dd 0x3
  dd -(0x1BADB002 + 0x3)

section .text
global _start
_start:
  cli
  ; A real GDT: the multiboot loader hands us flat segment caches but no
  ; table in memory, and exception delivery reloads CS through the GDT.
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
  jmp 0x08:.reload_cs
.reload_cs:

  ; Minimal IDT: #GP and #PF report the vector/error/EIP/CR2/CR0/CR3 to
  ; FAULT and leave through the debug-exit port. A benchmark should not fault;
  ; if it does, the six dwords are the whole story.
  mov word [IDTPTR], 0x7ff
  mov dword [IDTPTR + 2], IDT
  xor eax, eax
  mov ecx, 0x400
  mov edi, IDT
  rep stosd
  mov eax, gp_handler
  mov [IDT + 13 * 8], ax
  mov word [IDT + 13 * 8 + 2], 0x08
  mov byte [IDT + 13 * 8 + 4], 0
  mov byte [IDT + 13 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + 13 * 8 + 6], ax
  mov eax, pf_handler
  mov [IDT + 14 * 8], ax
  mov word [IDT + 14 * 8 + 2], 0x08
  mov byte [IDT + 14 * 8 + 4], 0
  mov byte [IDT + 14 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + 14 * 8 + 6], ax
  lidt [IDTPTR]

  ; PD[0] -> PT0, PD[1] -> PT1, both present | writable.
  mov dword [PD], PT0 | 3
  mov dword [PD + 4], PT1 | 3

  ; PT0[i] = i*4K | 3 for i = 0..1023 (0-4 MiB).
  mov edi, PT0
  xor ecx, ecx
.fill0:
  mov eax, ecx
  shl eax, 12
  or eax, 3
  mov [edi + ecx * 4], eax
  inc ecx
  cmp ecx, 1024
  jb .fill0

  ; PT1[i] = (1024+i)*4K | 3 (4-8 MiB).
  mov edi, PT1
  xor ecx, ecx
.fill1:
  lea eax, [ecx + 1024]
  shl eax, 12
  or eax, 3
  mov [edi + ecx * 4], eax
  inc ecx
  cmp ecx, 1024
  jb .fill1

  mov eax, PD
  mov cr3, eax
  mov eax, cr0
  or eax, 0x80000000
  mov cr0, eax

  ; Phase 1: 256 pages, fits the TLB.
  mov ecx, NPHASE1
.phase1:
  mov edx, ecx
  and edx, 0xff
  shl edx, 12
  mov eax, [BUF + edx]
  add [BUF + edx], eax
  dec ecx
  jnz .phase1

  ; Phase 2: 1024 pages, thrashes the TLB.
  mov ecx, NPHASE2
.phase2:
  mov edx, ecx
  and edx, 0x3ff
  shl edx, 12
  mov eax, [BUF + edx]
  add [BUF + edx], eax
  dec ecx
  jnz .phase2

  mov al, 5
  mov dx, 0xf4
  out dx, al
.hang:
  hlt
  jmp .hang

gp_handler:
  mov dword [FAULT], 13
  jmp fault_common
pf_handler:
  mov dword [FAULT], 14
fault_common:
  mov eax, [esp]        ; error code
  mov [FAULT + 4], eax
  mov eax, [esp + 4]    ; faulting EIP
  mov [FAULT + 8], eax
  mov eax, cr2
  mov [FAULT + 12], eax
  mov eax, cr0
  mov [FAULT + 16], eax
  mov eax, cr3
  mov [FAULT + 20], eax
  mov al, 5
  mov dx, 0xf4
  out dx, al
.hang_fault:
  hlt
  jmp .hang_fault
