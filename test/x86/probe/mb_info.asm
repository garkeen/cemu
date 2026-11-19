; Multiboot 1 info handoff probe (self-contained multiboot ELF).
;
; cemu enters a multiboot image with EAX=0x2BADB002 and EBX pointing at the
; machine description the board publishes (0x9f000). This probe checks the
; fields the board promises: flags, mem_lower/mem_upper, the e820 map and the
; loader name. Pass exits through isa-debug-exit 0xF4 with payload 5 (host
; status 11); each failed check has its own payload.
;
; Build: nasm -f elf32 -o mb_info.o mb_info.asm
;        ld.lld -m elf_i386 -T ../pm/pm_smoke.ld -o mb_info.elf mb_info.o
; Run:   timeout 30 cemu --machine x86 --isa x86 mb_info.elf

bits 32

section .multiboot
align 4
  dd 0x1BADB002
  dd 0x3
  dd -(0x1BADB002 + 0x3)

section .text
global _start
_start:
  cli

  ; The loader magic and the info pointer (Multiboot 0.6.96 §3.2).
  cmp eax, 0x2BADB002
  jne fail1
  test ebx, ebx
  jz fail2
  cmp ebx, 0x9f000
  jne fail2
  mov esi, ebx

  ; flags: memory (bit 0), mmap (bit 6), loader name (bit 9).
  mov eax, [esi]
  and eax, 0x241
  cmp eax, 0x241
  jne fail3

  ; 0x9fc00 bytes below 1 MiB = 639 KiB.
  cmp dword [esi + 4], 639
  jne fail4

  ; The map is three or four entries of 24 bytes (the size field of each is
  ; 20, so the stride is 24). A machine above 1 MiB has a fourth.
  mov eax, [esi + 0x2c]
  cmp eax, 72
  jb fail5
  cmp eax, 96
  ja fail5
  mov edi, [esi + 0x30]
  test edi, edi
  jz fail5

  ; Entry 0: 0..0x9fc00, usable.
  cmp dword [edi], 20
  jne fail6
  cmp dword [edi + 4], 0
  jne fail6
  cmp dword [edi + 8], 0
  jne fail6
  cmp dword [edi + 12], 0x9fc00
  jne fail6
  cmp dword [edi + 16], 0
  jne fail6
  cmp dword [edi + 20], 1
  jne fail6

  ; Entry 1: 0x9fc00..0xa0000, the EBDA, reserved.
  cmp dword [edi + 24], 20
  jne fail7
  cmp dword [edi + 28], 0x9fc00
  jne fail7
  cmp dword [edi + 32], 0
  jne fail7
  cmp dword [edi + 36], 0x400
  jne fail7
  cmp dword [edi + 40], 0
  jne fail7
  cmp dword [edi + 44], 2
  jne fail7

  ; Entry 2: 0xf0000..0x100000, the BIOS window, reserved.
  cmp dword [edi + 48], 20
  jne fail8
  cmp dword [edi + 52], 0xf0000
  jne fail8
  cmp dword [edi + 56], 0
  jne fail8
  cmp dword [edi + 60], 0x10000
  jne fail8
  cmp dword [edi + 64], 0
  jne fail8
  cmp dword [edi + 68], 2
  jne fail8

  ; A fourth entry, when present, is RAM above 1 MiB and must agree with
  ; mem_upper (both are KiB below/above the same boundary).
  cmp dword [esi + 0x2c], 96
  je .fourth
  ; Three entries means the machine ends at 1 MiB; mem_upper must be zero.
  cmp dword [esi + 8], 0
  jne fail9
  jmp .no_fourth
.fourth:
  cmp dword [edi + 72], 20
  jne fail9
  cmp dword [edi + 76], 0x100000
  jne fail9
  cmp dword [edi + 80], 0
  jne fail9
  cmp dword [edi + 88], 0
  jne fail9
  cmp dword [edi + 92], 1
  jne fail9
  mov eax, [esi + 8]
  shl eax, 10
  cmp dword [edi + 84], eax
  jne fail9
.no_fourth:

  ; boot_loader_name is a NUL-terminated "cemu".
  mov edi, [esi + 0x40]
  test edi, edi
  jz fail10
  cmp dword [edi], 0x756D6563  ; 'c','e','m','u'
  jne fail10
  cmp byte [edi + 4], 0
  jne fail10

  mov al, 5
  out 0xf4, al
  hlt

fail1:  mov al, 1
        jmp fail
fail2:  mov al, 2
        jmp fail
fail3:  mov al, 3
        jmp fail
fail4:  mov al, 4
        jmp fail
fail5:  mov al, 6
        jmp fail
fail6:  mov al, 7
        jmp fail
fail7:  mov al, 8
        jmp fail
fail8:  mov al, 9
        jmp fail
fail9:  mov al, 10
        jmp fail
fail10: mov al, 11
fail:
  out 0xf4, al
  hlt
