; i686 system-feature probe (self-contained multiboot ELF): RDTSC/RDTSCP,
; the MSR bank (TSC, SYSENTER, MTRR/PAT, MCE, performance counters), SYSENTER
; and the CPUID advertisement. Pass exits via isa-debug-exit 0xF4 payload 5.
;
; Build: nasm -f elf32 -o sys686.o sys686.asm
;        ld.lld -m elf_i386 -T ../pm/pm_smoke.ld -o sys686.elf sys686.o
; Run:   timeout 30 cemu --machine x86 --isa x86 sys686.elf

bits 32

%define GDT     0x2000
%define GDTPTR  0x1f00
%define IDT     0x3000
%define IDTPTR  0x2f00
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
  mov eax, bad_handler
  call set_gate6
  mov eax, bad_handler
  call set_gate13
  lidt [IDTPTR]

  ; CPUID leaf 1 advertises TSC, MSR, MTRR, PAT, PSE36, MCE/SEP; leaf 0x0a
  ; reports two general and three fixed counters.
  mov eax, 1
  cpuid
  test edx, 1 << 4
  jz fail1
  test edx, 1 << 5
  jz fail1
  test edx, 1 << 11
  jz fail1
  test edx, 1 << 12
  jz fail1
  test edx, 1 << 16
  jz fail1
  test edx, 1 << 17
  jz fail1
  mov eax, 0x0a
  cpuid
  cmp eax, 0x0101
  jne fail1
  cmp edx, 2
  jne fail1

  ; APIC advertisement and topology: leaf 1 names one logical processor with
  ; APIC ID 0, leaf 0x0b walks SMT -> core -> end, and the LAPIC's ID
  ; register agrees (device/intc/lapic.c).
  mov eax, 1
  cpuid
  test edx, 1 << 9
  jz fail10
  mov eax, ebx
  shr eax, 24
  test eax, eax
  jnz fail10
  shr ebx, 16
  and ebx, 0xff
  cmp ebx, 1
  jne fail10
  mov eax, 0x0b
  xor ecx, ecx
  cpuid
  cmp ebx, 1
  jne fail10
  cmp ecx, 0x0100        ; level 0, type 1 = SMT
  jne fail10
  mov eax, 0x0b
  mov ecx, 1
  cpuid
  cmp ebx, 1
  jne fail10
  mov edx, ecx
  and edx, 0xff00
  cmp edx, 0x0200
  jne fail10
  mov eax, 0x0b
  mov ecx, 2
  cpuid
  test ebx, ebx
  jnz fail10
  mov eax, [0xfee00020]
  shr eax, 24
  test eax, eax
  jnz fail10

  ; RDTSC monotonic.
  rdtsc
  mov [B], eax
  mov [B + 4], edx
  rdtsc
  cmp edx, [B + 4]
  jb fail2
  ja .tsc_ok
  cmp eax, [B]
  jb fail2
.tsc_ok:

  ; WRMSR IA32_TSC = 1<<32; RDTSC then has EDX=1 (a few ticks may follow).
  mov ecx, 0x10
  xor eax, eax
  mov edx, 1
  wrmsr
  rdtsc
  cmp edx, 1
  jne fail3

  ; RDTSCP returns IA32_TSC_AUX in ECX.
  mov ecx, 0xc0000103
  mov eax, 0x1234
  xor edx, edx
  wrmsr
  rdtscp
  cmp ecx, 0x1234
  jne fail4
  cmp edx, 1
  jne fail4

  ; MTRR/PAT registers.
  mov ecx, 0xfe
  rdmsr
  cmp eax, 0x501
  jne fail5
  mov ecx, 0x200
  mov eax, 0x12345006
  xor edx, edx
  wrmsr
  mov ecx, 0x200
  rdmsr
  cmp eax, 0x12345006
  jne fail5
  mov ecx, 0x201
  mov eax, 0xfff00800
  xor edx, edx
  wrmsr
  mov ecx, 0x201
  rdmsr
  cmp eax, 0xfff00800
  jne fail5
  mov ecx, 0x2ff
  mov eax, 0xc06
  xor edx, edx
  wrmsr
  mov ecx, 0x2ff
  rdmsr
  cmp eax, 0xc06
  jne fail5
  mov ecx, 0x277
  mov eax, 0x03020100
  mov edx, 0x07060504
  wrmsr
  mov ecx, 0x277
  rdmsr
  cmp eax, 0x03020100
  jne fail5
  cmp edx, 0x07060504
  jne fail5

  ; MCE register bank.
  mov ecx, 0x179
  rdmsr
  cmp eax, 0x101
  jne fail6
  mov ecx, 0x17b
  mov eax, 1
  xor edx, edx
  wrmsr
  mov ecx, 0x17b
  rdmsr
  cmp eax, 1
  jne fail6
  mov ecx, 0x401
  rdmsr
  test eax, eax
  jnz fail6
  test edx, edx
  jnz fail6

  ; General performance counter 0 counts instructions retired (the global
  ; control register must enable it first, SDM vol.3 19.2.5).
  mov ecx, 0x38f
  mov eax, 3
  xor edx, edx
  wrmsr
  mov ecx, 0x186
  mov eax, (1 << 22) | (1 << 16) | 0xc0
  xor edx, edx
  wrmsr
  mov ecx, 0xc1
  xor eax, eax
  xor edx, edx
  wrmsr
  mov ecx, 0xc1
  rdmsr
  mov [B], eax
  mov [B + 4], edx
  mov ecx, 2000
.loop:
  nop
  dec ecx
  jnz .loop
  mov ecx, 0xc1
  rdmsr
  sub eax, [B]
  sbb edx, [B + 4]
  test edx, edx
  jnz .perf_ok
  cmp eax, 2000
  jb fail7
.perf_ok:

  ; The fixed instruction-retired counter follows the same event.
  mov ecx, 0x38d
  mov eax, 1
  xor edx, edx
  wrmsr
  mov ecx, 0x38f
  xor eax, eax
  mov edx, 1          ; bit 32: fixed counter 0
  wrmsr
  mov ecx, 0x309
  rdmsr
  mov [B], eax
  mov [B + 4], edx
  mov ecx, 100
.floop:
  nop
  dec ecx
  jnz .floop
  mov ecx, 0x309
  rdmsr
  sub eax, [B]
  sbb edx, [B + 4]
  test edx, edx
  jnz .fixed_ok
  cmp eax, 100
  jb fail8
.fixed_ok:

  ; SYSENTER: flat CS 0x08/SS 0x10, ESP and EIP from the MSRs, IF cleared.
  mov ecx, 0x174
  mov eax, 0x08
  xor edx, edx
  wrmsr
  mov ecx, 0x175
  mov eax, 0x70000
  xor edx, edx
  wrmsr
  mov ecx, 0x176
  mov eax, sysenter_target
  xor edx, edx
  wrmsr
  sti
  nop
  sysenter

sysenter_target:
  cmp esp, 0x70000
  jne fail9
  pushfd
  pop eax
  test eax, 0x200
  jnz fail9
  mov esp, 0x6f00
  cli

  mov al, 5
  out 0xf4, al
  hlt

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
fail:
  out 0xf4, al
  hlt
