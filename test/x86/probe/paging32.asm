; PAE / PGE / NX / cross-page probe (self-contained multiboot ELF).
;
; Builds a three-level PAE address space, turns on PAE|PGE, and checks:
;   t1  4KB PAE leaf: write/read through PDPTE -> PDE -> PTE
;   t2  2MB PAE leaf: write/read at VA 0x800000 (PDE[4] = PA 16MB | P|RW|PS)
;   t3  PAE A/D: a read sets PTE.A and leaves D clear, a write sets PTE.D
;   t4  PGE: a global translation survives a CR3 reload, a non-global one does
;       not; INVLPG then drops even the global one
;   t5  an unaligned 4-byte access crossing a 4KB boundary whose two pages map
;       to non-contiguous physical pages (reads and writes)
;   t6  NX: a fetch from a PAE PTE with bit 63 set faults with #PF.I=1 after
;       IA32_EFER.NXE is enabled (the #PF handler recognizes the expected fault)
;
; Exit: pass payload 5 (status 11); failure payload 1 (status 3). The fault
; record (vector, error, EIP, CR2) lands at FAULT for CEMU_DEBUG=dump=.
;
; Build: nasm -f elf32 -o paging32.o paging32.asm
;        ld.lld -m elf_i386 -T ../pm/pm_smoke.ld -o paging32.elf paging32.o

bits 32

%define PDPT      0x60000
%define PD        0x61000
%define PT0       0x62000
%define PT1       0x63000
%define GDT       0x2000
%define GDTPTR    0x1f00
%define IDT       0x3000
%define IDTPTR    0x2f00
%define EXPECT_NX 0x9000
%define FAULT     0x9010

%macro SET_GATE 2
  mov eax, %2
  mov [IDT + %1 * 8], ax
  mov word [IDT + %1 * 8 + 2], 0x08
  mov byte [IDT + %1 * 8 + 4], 0
  mov byte [IDT + %1 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + %1 * 8 + 6], ax
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

  ; GDT: null, flat code 0x08, flat data 0x10.
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

  ; IDT: #PF and #GP captured; every other vector funnels to fail_handler.
  mov word [IDTPTR], 0x7ff
  mov dword [IDTPTR + 2], IDT
  xor eax, eax
  mov ecx, 0x400
  mov edi, IDT
  rep stosd
  SET_GATE 13, gp_handler
  SET_GATE 14, pf_handler
  SET_GATE 0, fail_handler
  SET_GATE 6, fail_handler
  SET_GATE 8, fail_handler
  SET_GATE 10, fail_handler
  SET_GATE 11, fail_handler
  SET_GATE 12, fail_handler
  SET_GATE 17, fail_handler
  lidt [IDTPTR]

  mov dword [EXPECT_NX], 0

  ; Zero the PDPT and PD, then build the three-level tables.
  xor eax, eax
  mov ecx, 8
  mov edi, PDPT
  rep stosd
  mov ecx, 0x400
  mov edi, PD
  rep stosd

  mov dword [PDPT], PD | 3
  mov dword [PD], PT0 | 3
  mov dword [PD + 8], PT1 | 3
  mov dword [PD + 4 * 8], 0x01000000 | 0x83   ; 2MB leaf VA 8MB -> PA 16MB

  ; PT0 maps 0-2MB identity; PT1 maps 2-4MB identity (8-byte PAE entries).
  mov edi, PT0
  xor ecx, ecx
.fill0:
  mov eax, ecx
  shl eax, 12
  or eax, 3
  mov [edi + ecx * 8], eax
  mov dword [edi + ecx * 8 + 4], 0
  inc ecx
  cmp ecx, 512
  jb .fill0

  mov edi, PT1
  xor ecx, ecx
.fill1:
  lea eax, [ecx + 512]
  shl eax, 12
  or eax, 3
  mov [edi + ecx * 8], eax
  mov dword [edi + ecx * 8 + 4], 0
  inc ecx
  cmp ecx, 512
  jb .fill1

  ; VA 0x300000 is the global page; 0x301000 is its non-global partner.
  or dword [PT1 + 0x100 * 8], 0x100        ; G on 0x300000
  ; VA 0x321000 points at PA 0x330000 (non-contiguous with 0x320000).
  mov dword [PT1 + 0x121 * 8], 0x330000 | 3
  mov dword [PT1 + 0x121 * 8 + 4], 0

  ; PAE + PGE, then PG.
  mov eax, cr4
  or eax, 0x20 | 0x80
  mov cr4, eax
  mov eax, PDPT
  mov cr3, eax
  mov eax, cr0
  or eax, 0x80000000
  mov cr0, eax

  ; t1: 4KB PAE leaf.
  mov dword [0x200000], 0x12345678
  cmp dword [0x200000], 0x12345678
  jne fail

  ; t2: 2MB PAE leaf.
  mov dword [0x800000], 0xABCDEF01
  cmp dword [0x800000], 0xABCDEF01
  jne fail

  ; t2b: INVLPG of one 4KB subpage invalidates the whole 2MB leaf. Remap
  ; PDE[4] to PA 18MB and check that the 0x800000 slot no longer sees the old
  ; 0xABCDEF01 page.
  mov eax, [0x801000]
  mov dword [PD + 4 * 8], 0x01200000 | 0x83
  invlpg [0x801000]
  mov eax, [0x800000]
  cmp eax, 0xABCDEF01
  je fail
  cmp eax, 0
  jne fail

  ; t3: PAE A/D on a 4KB leaf (A on read, D on write).
  and dword [PT1 + 1 * 8], ~0x60
  mov eax, PDPT
  mov cr3, eax
  mov eax, [0x201000]
  mov edx, [PT1 + 1 * 8]
  test edx, 0x20
  jz fail
  test edx, 0x40
  jnz fail
  and dword [PT1 + 1 * 8], ~0x60
  mov eax, PDPT
  mov cr3, eax
  mov dword [0x201000], 1
  mov edx, [PT1 + 1 * 8]
  and edx, 0x60
  cmp edx, 0x60
  jne fail

  ; t4: PGE. Markers first, then fill the TLB, then remap and reload CR3.
  mov dword [0x300000], 0x11111111
  mov dword [0x310000], 0x22222222
  mov dword [0x301000], 0x33333333
  mov dword [0x311000], 0x44444444
  mov eax, [0x300000]
  mov eax, [0x301000]
  mov dword [PT1 + 0x100 * 8], 0x310000 | 3 | 0x100
  mov dword [PT1 + 0x101 * 8], 0x311000 | 3
  mov eax, PDPT
  mov cr3, eax
  mov eax, [0x300000]
  cmp eax, 0x11111111          ; global entry survived
  jne fail
  mov eax, [0x301000]
  cmp eax, 0x44444444          ; non-global entry was flushed
  jne fail
  invlpg [0x300000]            ; INVLPG drops the global entry too
  mov eax, [0x300000]
  cmp eax, 0x22222222
  jne fail

  ; t5: unaligned 4-byte access crossing 0x321000 (PA 0x330000).
  mov byte [0x320FFE], 0x11
  mov byte [0x320FFF], 0x22
  mov byte [0x330000], 0x33
  mov byte [0x330001], 0x44
  cmp dword [0x320FFE], 0x44332211
  jne fail
  mov dword [0x320FFE], 0x88776655
  cmp byte [0x320FFE], 0x55
  jne fail
  cmp byte [0x320FFF], 0x66
  jne fail
  cmp byte [0x330000], 0x77
  jne fail
  cmp byte [0x330001], 0x88
  jne fail

  ; t6: NX. Enable EFER.NXE, mark 0x202000 non-executable, then call into it.
  mov ecx, 0xC0000080
  mov eax, 0x800
  xor edx, edx
  wrmsr
  mov byte [0x202000], 0xC3    ; a ret, if the CPU wrongly executes it
  or dword [PT1 + 2 * 8 + 4], 0x80000000
  invlpg [0x202000]
  mov dword [EXPECT_NX], 1
  call 0x202000
  jmp fail

pass:
  mov al, 5
  mov dx, 0xf4
  out dx, al
.hang_pass:
  hlt
  jmp .hang_pass

fail:
  mov al, 1
  mov dx, 0xf4
  out dx, al
.hang_fail:
  hlt
  jmp .hang_fail

gp_handler:
  mov dword [FAULT], 13
  jmp gp_common
pf_handler:
  mov dword [FAULT], 14
  mov eax, [esp]
  mov [FAULT + 4], eax
  mov eax, [esp + 4]
  mov [FAULT + 8], eax
  mov eax, cr2
  mov [FAULT + 12], eax
  cmp dword [EXPECT_NX], 0
  je fail
  mov eax, [FAULT + 4]
  test eax, 0x10
  jz fail
  jmp pass

gp_common:
  mov eax, [esp]
  mov [FAULT + 4], eax
  mov eax, [esp + 4]
  mov [FAULT + 8], eax
  mov eax, cr2
  mov [FAULT + 12], eax
  jmp fail

fail_handler:
  mov dword [FAULT], 0xff
  mov eax, [esp]
  mov [FAULT + 4], eax
  mov eax, [esp + 4]
  mov [FAULT + 8], eax
  mov eax, cr2
  mov [FAULT + 12], eax
  jmp fail
