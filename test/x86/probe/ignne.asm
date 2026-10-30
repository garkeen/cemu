; IGNNE#, the chipset's port 0xF0 latch, observed through the IRQ13 the CPU
; would otherwise raise (not an acceptance test -- a probe that pins one
; CPU/chipset handshake).
;
; With CR0.NE=0 the processor reports an unmasked x87 exception the MS-DOS way:
; it drives FERR# low, the board turns that into the coprocessor-error IRQ13,
; and port 0xF0's IGNNE# latch makes the processor ignore the error instead.
; The latch lasts as long as the error does: loading an FPU environment whose
; status word reports no error drops it (FNCLEX and FINIT do not -- the
; reference keeps the latch through both, and so does this machine).
;
; IRQ13 cannot be made level-sensitive (the PIIX3's ELCR does not expose that
; line; both models carry the same mask), so the probe counts deliveries through
; a real interrupt gate instead of watching a pending bit:
;
;   a   one divide-by-zero with IGNNE# released: IRQ13 is delivered once
;   b   the same with IGNNE# asserted by a port 0xF0 write: nothing is
;       delivered
;   c   FNCLEX clears the error, FLDENV clears the latch, and a third
;       divide-by-zero is delivered again -- the latch really was dropped
;
; Reference: SDM vol.1 8.7.2 (FERR#/IGNNE# with CR0.NE=0); QEMU hw/i386/pc.c
; ioportF0_write (any write asserts it, the data byte ignored) and
; target/i386/tcg/system/fpu_helper.c (fpu_check_raise_ferr_irq skips the raise
; while HF2_IGNNE is set, cpu_set_ignne lowers ferr_irq, cpu_clear_ignne runs
; from cpu_set_fpus -- which only FLDENV/FRSTOR reach).
;
; Build: nasm -f elf32 -o ignne.o ignne.asm
;        ld.lld -m elf_i386 -T vbe_mode.ld -o ignne.elf ignne.o
; Run:   cemu --machine x86 --isa x86 ignne.elf
;        qemu-system-i386 -machine pc -m 32 -kernel ignne.elf -display none \
;            -no-reboot -device isa-debug-exit,iobase=0xf4,iosize=0x4 \
;            -serial file:ignne_qemu.txt
; Exit: debug-exit 5 (status 11) once the transcript is out.

  bits 32

%define DBGEXIT   0xf4
%define PORT_F0   0xf0
%define IDT_BASE  0x20000     ; a page of RAM clear of this image
%define SPIN      0x200000    ; per-phase budget, in iterations

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

  ; CR0.NE=0 (FERR# reporting instead of #MF), EM and TS clear so the FPU runs.
  mov eax, cr0
  and eax, ~(0x20 | 0x08 | 0x04)
  mov cr0, eax


  ; A real GDT: a multiboot image enters on built-in descriptors with no GDT
  ; behind them, and an interrupt gate's selector has to name a descriptor
  ; (cemu answers that delivery with #GP before this runs).
  mov word [gdt_desc], 3 * 8 - 1
  mov dword [gdt_desc + 2], gdt
  lgdt [gdt_desc]
  mov ax, 0x10                  ; flat data
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  mov ss, ax
  jmp 0x08:.reloaded            ; flat code
.reloaded:
  ; An IDT whose every vector is a bare IRET, with vector 0x2D for IRQ13 once
  ; the 8259 pair is remapped to 0x20/0x28.
  mov edi, IDT_BASE
  mov ecx, 256
.fill_idt:
  mov eax, isr_default
  mov [edi], ax
  mov word [edi + 2], 0x08      ; the loader's flat code selector
  mov byte [edi + 4], 0
  mov byte [edi + 5], 0x8e      ; present, DPL 0, 32-bit interrupt gate
  shr eax, 16
  mov [edi + 6], ax
  add edi, 8
  loop .fill_idt
  mov edi, IDT_BASE + 0x2d * 8
  mov eax, isr_fpu
  mov [edi], ax
  shr eax, 16
  mov [edi + 6], ax
  mov word [idt_desc], 0x7ff
  mov dword [idt_desc + 2], IDT_BASE
  lidt [idt_desc]

  ; 8259 pair: vectors 0x20/0x28, 8086 mode, only the cascade and IRQ13 open.
  mov al, 0x11
  out 0x20, al
  out 0xa0, al
  mov al, 0x20
  out 0x21, al
  mov al, 0x28
  out 0xa1, al
  mov al, 0x04
  out 0x21, al
  mov al, 0x02
  out 0xa1, al
  mov al, 0x01
  out 0x21, al
  out 0xa1, al
  mov al, 0xfb                  ; master: IRQ2 (the cascade) open
  out 0x21, al
  mov al, 0xdf                  ; slave: IRQ5 = IRQ13 open
  out 0xa1, al

  ; The control word the phases run with: only ZE unmasked.
  fninit
  fldcw [cw_ze]

  ; a) The error reaches IRQ13.
  fld1
  fdiv dword [zero]
  fwait
  sti
  mov ecx, SPIN
.wait_a:
  cmp dword [count], 1
  jae .done_a
  dec ecx
  jnz .wait_a
.done_a:
  cli
  P m_a
  mov al, [count]
  call hex8

  ; b) IGNNE# asserted: the processor ignores the error, so nothing arrives.
  mov dx, PORT_F0
  xor al, al
  out dx, al
  fld1
  fdiv dword [zero]
  fwait
  sti
  mov ecx, SPIN
.wait_b:
  dec ecx
  jnz .wait_b
  cli
  P m_b
  mov al, [count]
  call hex8

  ; c) FNCLEX clears the flags, FLDENV clears the latch, and the next error is
  ;    delivered again -- so the latch is not simply stuck.
  fnclex
  fldenv [env_clear]
  fldcw [cw_ze]
  fld1
  fdiv dword [zero]
  fwait
  sti
  mov ecx, SPIN
.wait_c:
  cmp dword [count], 2
  jae .done_c
  dec ecx
  jnz .wait_c
.done_c:
  cli
  P m_c
  mov al, [count]
  call hex8

  P m_ok
  P m_crlf

  mov dx, DBGEXIT
  mov al, 5
  out dx, al
  jmp $

; IRQ13's handler: count it, clear the exception flags so FERR# drops and the
; next error is a fresh edge, and acknowledge both chips (the slave's request
; reached the processor through the master's cascade).
isr_fpu:
  push eax
  inc dword [count]
  fnclex
  mov al, 0x20
  out 0xa0, al
  out 0x20, al
  pop eax
  iretd

isr_default:
  iretd

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
align 4
zero:      dd 0.0
cw_ze:     dw 0x037b            ; default 0x037f with the zero-divide mask off
count:     dd 0
idt_desc:  dw 0
           dd 0
; The GDT the gate's selector names: null, flat 32-bit code, flat data.
gdt:       dq 0
           dq 0x00cf9a000000ffff
           dq 0x00cf92000000ffff
gdt_desc:  dw 0
           dd 0
; 32-bit FLDENV image with no error in its status word (FCW, FSW, FTW, FIP,
; FCS:FOP, FDP, FDS), 28 bytes.
env_clear: dd 0x037b
           dd 0
           dd 0xffff
           dd 0
           dd 0
           dd 0
           dd 0
m_a:       db "ignne a=", 0
m_b:       db " b=", 0
m_c:       db " c=", 0
m_ok:      db " ok", 0
m_crlf:    db 13, 10, 0
