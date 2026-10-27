; MMX/SSE/SSE2 probe (self-contained multiboot ELF). Covers the block moves,
; packed/scalar float arithmetic, conversions, compares, shuffles, the
; packed-integer engine, MXCSR and FXSAVE/FXRSTOR of the XMM state.
; Pass exits via isa-debug-exit 0xF4 payload 5; each failure has its own code.
;
; Build: nasm -f elf32 -o sse.o sse.asm
;        ld.lld -m elf_i386 -T ../pm/pm_smoke.ld -o sse.elf sse.o
; Run:   timeout 30 cemu --machine x86 --isa x86 sse.elf

bits 32

%define GDT     0x2000
%define GDTPTR  0x1f00
%define IDT     0x3000
%define IDTPTR  0x2f00
%define B       0x80000
%define FX      0x81000

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
  mov eax, bad_handler
  mov [IDT + 6 * 8], ax
  mov word [IDT + 6 * 8 + 2], 0x08
  mov byte [IDT + 6 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + 6 * 8 + 6], ax
  mov eax, bad_handler
  mov [IDT + 13 * 8], ax
  mov word [IDT + 13 * 8 + 2], 0x08
  mov byte [IDT + 13 * 8 + 5], 0x8e
  shr eax, 16
  mov [IDT + 13 * 8 + 6], ax
  lidt [IDTPTR]

  mov eax, cr4
  or eax, 0x600        ; OSFXSR | OSXMMEXCPT
  mov cr4, eax
  mov dword [B], 0x04030201
  mov dword [B + 4], 0x08070605
  mov dword [B + 8], 0x0c0b0a09
  mov dword [B + 12], 0x100f0e0d

  ; 1. MOVDQU round trip.
  movdqu xmm0, [B]
  movdqu [B + 16], xmm0
  mov eax, [B + 16]
  cmp eax, 0x04030201
  jne fail1
  mov eax, [B + 28]
  cmp eax, 0x100f0e0d
  jne fail1

  ; 2. PADDD (1,2,3,4)+(10,20,30,40) = (11,22,33,44).
  mov dword [B], 1
  mov dword [B + 4], 2
  mov dword [B + 8], 3
  mov dword [B + 12], 4
  mov dword [B + 16], 10
  mov dword [B + 20], 20
  mov dword [B + 24], 30
  mov dword [B + 28], 40
  movdqu xmm0, [B]
  movdqu xmm1, [B + 16]
  paddd xmm0, xmm1
  movdqu [B + 32], xmm0
  cmp dword [B + 32], 11
  jne fail2
  cmp dword [B + 44], 44
  jne fail2

  ; 3. ADDPS 1.5 + 2.25 = 3.75 (0x40700000).
  mov dword [B], 0x3fc00000
  mov dword [B + 4], 0x3fc00000
  mov dword [B + 8], 0x3fc00000
  mov dword [B + 12], 0x3fc00000
  mov dword [B + 16], 0x40100000
  mov dword [B + 20], 0x40100000
  mov dword [B + 24], 0x40100000
  mov dword [B + 28], 0x40100000
  movups xmm0, [B]
  movups xmm1, [B + 16]
  addps xmm0, xmm1
  movups [B + 32], xmm0
  cmp dword [B + 32], 0x40700000
  jne fail3

  ; 4. ADDPD: the same in doubles.
  mov dword [B], 0
  mov dword [B + 4], 0x3ff80000
  mov dword [B + 8], 0
  mov dword [B + 12], 0x3ff80000
  mov dword [B + 16], 0
  mov dword [B + 20], 0x40020000
  mov dword [B + 24], 0
  mov dword [B + 28], 0x40020000
  movupd xmm0, [B]
  movupd xmm1, [B + 16]
  addpd xmm0, xmm1
  movupd [B + 32], xmm0
  cmp dword [B + 32], 0
  jne fail4
  cmp dword [B + 36], 0x400e0000
  jne fail4

  ; 5. SQRTPS(4.0) = 2.0.
  mov dword [B], 0x40800000
  mov dword [B + 4], 0x40800000
  mov dword [B + 8], 0x40800000
  mov dword [B + 12], 0x40800000
  movups xmm0, [B]
  sqrtps xmm0, xmm0
  movups [B + 16], xmm0
  cmp dword [B + 16], 0x40000000
  jne fail5

  ; 6. CVTDQ2PS then CVTPS2DQ round trip.
  mov dword [B], 1
  mov dword [B + 4], 2
  mov dword [B + 8], 3
  mov dword [B + 12], 4
  movdqu xmm0, [B]
  cvtdq2ps xmm0, xmm0
  cvtps2dq xmm0, xmm0
  movdqu [B + 16], xmm0
  cmp dword [B + 16], 1
  jne fail6
  cmp dword [B + 28], 4
  jne fail6

  ; 7. CMPEQPS: (1,2,3,4) vs (1,9,3,9).
  mov dword [B], 1
  mov dword [B + 4], 2
  mov dword [B + 8], 3
  mov dword [B + 12], 4
  mov dword [B + 16], 1
  mov dword [B + 20], 9
  mov dword [B + 24], 3
  mov dword [B + 28], 9
  movups xmm0, [B]
  movups xmm1, [B + 16]
  cmpeqps xmm0, xmm1
  movups [B + 32], xmm0
  cmp dword [B + 32], 0xffffffff
  jne fail7
  cmp dword [B + 36], 0
  jne fail7
  cmp dword [B + 40], 0xffffffff
  jne fail7

  ; 8. PSHUFD reverse (imm 0x1b).
  mov dword [B], 1
  mov dword [B + 4], 2
  mov dword [B + 8], 3
  mov dword [B + 12], 4
  movdqu xmm0, [B]
  pshufd xmm1, xmm0, 0x1b
  movdqu [B + 16], xmm1
  cmp dword [B + 16], 4
  jne fail8
  cmp dword [B + 28], 1
  jne fail8

  ; 9. COMISS 1.0 vs 2.0: CF=1, ZF=0, PF=0.
  mov dword [B], 0x3f800000
  mov dword [B + 4], 0x40000000
  movss xmm0, [B]
  comiss xmm0, [B + 4]
  pushfd
  pop eax
  test eax, 1
  jz fail9
  test eax, 0x40
  jnz fail9

  ; 10. PMOVMSKB of 0x80,0x00,0x80,... -> 0x5555.
  mov byte [B], 0x80
  mov byte [B + 1], 0
  mov byte [B + 2], 0x80
  mov byte [B + 3], 0
  mov byte [B + 4], 0x80
  mov byte [B + 5], 0
  mov byte [B + 6], 0x80
  mov byte [B + 7], 0
  mov byte [B + 8], 0x80
  mov byte [B + 9], 0
  mov byte [B + 10], 0x80
  mov byte [B + 11], 0
  mov byte [B + 12], 0x80
  mov byte [B + 13], 0
  mov byte [B + 14], 0x80
  mov byte [B + 15], 0
  movdqu xmm0, [B]
  pmovmskb eax, xmm0
  cmp eax, 0x5555
  jne fail10

  ; 11. PUNPCKLBW + PACKSSWB: words 0x0102/0x0304 -> bytes 2,1,4,3.
  mov dword [B], 0x00000000
  mov ax, 0x0102
  mov [B], ax
  mov ax, 0x0304
  mov [B + 2], ax
  mov ax, 0x0506
  mov [B + 4], ax
  mov ax, 0x0708
  mov [B + 6], ax
  movq mm0, [B]
  movq mm1, [B]
  punpcklbw mm0, mm1
  movq [B + 16], mm0
  cmp byte [B + 16], 2
  jne fail11
  cmp byte [B + 17], 2
  jne fail11
  cmp byte [B + 18], 1
  jne fail11
  emms

  ; 12. PSLLD imm4 / PSRLD imm4 on {1,2,3,4}.
  mov dword [B], 1
  mov dword [B + 4], 2
  mov dword [B + 8], 3
  mov dword [B + 12], 4
  movdqu xmm0, [B]
  pslld xmm0, 4
  movdqu [B + 16], xmm0
  cmp dword [B + 16], 16
  jne fail12
  psrld xmm0, 4
  movdqu [B + 32], xmm0
  cmp dword [B + 32], 1
  jne fail12

  ; 13. PINSRW/PEXTRW.
  movdqu xmm0, [B]
  mov eax, 0xbeef
  pinsrw xmm0, eax, 5
  pextrw eax, xmm0, 5
  cmp eax, 0xbeef
  jne fail13

  ; 14. MXCSR read-back and FTZ/DAZ bits.
  mov dword [B], 0x1fc0
  ldmxcsr [B]
  stmxcsr [B + 4]
  cmp dword [B + 4], 0x1fc0
  jne fail14
  mov dword [B], 0x1f80
  ldmxcsr [B]

  ; 15. FXSAVE/FXRSTOR keeps XMM0.
  mov dword [B], 0x11223344
  movdqu xmm0, [B]
  fxsave [FX]
  pxor xmm0, xmm0
  fxrstor [FX]
  movdqu [B + 16], xmm0
  cmp dword [B + 16], 0x11223344
  jne fail15

  ; 16. MMX PADDB and EMMS.
  mov dword [B], 0x01020304
  movd mm0, [B]
  paddb mm0, mm0
  movd [B + 4], mm0
  cmp dword [B + 4], 0x02040608
  jne fail16
  emms

  ; 17. Signed float order: -1 > -2 (raw bit order would say the opposite).
  mov dword [B], 0xbf800000       ; -1.0
  mov dword [B + 4], 0xbf800000
  mov dword [B + 8], 0xbf800000
  mov dword [B + 12], 0xbf800000
  mov dword [B + 16], 0xc0000000  ; -2.0
  mov dword [B + 20], 0xc0000000
  mov dword [B + 24], 0xc0000000
  mov dword [B + 28], 0xc0000000
  movups xmm0, [B]
  movups xmm1, [B + 16]
  movaps xmm2, xmm0
  cmpltps xmm2, xmm1              ; -1 < -2 is false
  movups [B + 32], xmm2
  cmp dword [B + 32], 0
  jne fail17a
  movaps xmm2, xmm1
  cmpltps xmm2, xmm0              ; -2 < -1 is true
  movups [B + 32], xmm2
  cmp dword [B + 32], 0xffffffff
  jne fail17b
  movaps xmm2, xmm0
  minps xmm2, xmm1                ; min(-1,-2) = -2
  movups [B + 32], xmm2
  cmp dword [B + 32], 0xc0000000
  jne fail17c
  comiss xmm0, xmm1               ; -1 > -2 -> CF=0
  pushfd
  pop eax
  test eax, 1
  jnz fail17d

  ; 18. A denormal addend raises MXCSR.DE (masked, so no #XM).
  mov dword [B], 0x1f80
  ldmxcsr [B]
  mov dword [B], 1              ; smallest float subnormal
  mov dword [B + 4], 1
  mov dword [B + 8], 1
  mov dword [B + 12], 1
  mov dword [B + 16], 0x3f800000
  mov dword [B + 20], 0x3f800000
  mov dword [B + 24], 0x3f800000
  mov dword [B + 28], 0x3f800000
  movups xmm0, [B]
  movups xmm1, [B + 16]
  addps xmm0, xmm1
  stmxcsr [B + 32]
  test dword [B + 32], 2
  jz fail18
  mov dword [B], 0x1f80
  ldmxcsr [B]

  mov al, 5
  out 0xf4, al
  hlt

bad_handler:
  mov al, 0x0f
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
        jmp fail
fail11: mov al, 12
        jmp fail
fail12: mov al, 13
        jmp fail
fail13: mov al, 14
        jmp fail
fail14: mov al, 15
        jmp fail
fail15: mov al, 16
        jmp fail
fail16: mov al, 17
        jmp fail
fail17a: mov al, 18
         jmp fail
fail17b: mov al, 19
         jmp fail
fail17c: mov al, 20
         jmp fail
fail17d: mov al, 21
         jmp fail
fail18: mov al, 22
fail:
  out 0xf4, al
  hlt
