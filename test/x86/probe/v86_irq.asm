; V86 hardware-interrupt nesting probe (self-contained multiboot ELF at 0x10000).
;
; The DOS/EMM386 crash happens when a timer IRQ lands while the V86 monitor is
; reflecting an interrupt and the V86 handler is live. This probe reproduces
; that shape deterministically and prints the saved SS:ESP of every level:
;
;   IRQ 1  fires while the V86 main loop spins at SS:ESP = 0x1800:0x7000;
;          the ring-0 handler sends EOI, reflects to a V86 handler by writing
;          the interrupted CS:EIP:FLAGS at [SS:ESP-6] and IRETs with SP-6.
;   IRQ 2  fires while that V86 handler spins at 0x1800:0x6ffa -> saved ESP
;          must be 0x6ffa (the outer reflector frame is still live).
;   IRQ 3  nests once more -> saved ESP 0x6ff4.
;   The handlers unwind through the three reflected frames; the main loop
;   sees count 3 and exits.
;
; Run with -icount on both sides is not required: the spin design fixes the
; nesting, and only ESP/SS/CS/EFLAGS are printed (the interrupted EIP depends
; on arrival time).
;
; Build: nasm -f elf32 -o v86_irq.o v86_irq.asm
;        ld.lld -m elf_i386 -T v86_int.ld -o v86_irq.elf v86_irq.o
; Run:   cemu --machine x86 --isa x86 v86_irq.elf
;        qemu-system-i386 -kernel v86_irq.elf -display none \
;            -serial file:v86_irq_qemu.txt -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
bits 32

%define SEL_CODE0 0x08
%define SEL_DATA0 0x10
%define SEL_TSS   0x18

%define IDT_LIN   0x2100
%define TSS_LIN   0x2400
%define TSS_ESP0  0x4800
%define F8        0x2080
%define F21       0x2084
%define FGP       0x2088
%define IRQS      0x208c

%define V86_STUB  0x8000
%define V86_HAND  0x8200
%define V86_SS    0x1800
%define V86_SP    0x7000
%define MARK      0x9100
%define IRQ_TAB   0x2600

section .multiboot
align 4
  dd 0x1BADB002
  dd 0x3
  dd -(0x1BADB002 + 0x3)

section .text
global _start

_start:
  cli
  mov al, 0xff
  out 0x21, al
  out 0xa1, al

  mov dx, 0x3fb
  mov al, 0x80
  out dx, al
  mov dx, 0x3f8
  mov al, 1
  out dx, al
  mov dx, 0x3f9
  xor al, al
  out dx, al
  mov dx, 0x3fb
  mov al, 3
  out dx, al
  mov dx, 0x3f9
  xor al, al
  out dx, al

  ; 8259: ICW1-4 (master base 8, slave base 0x70, 8086 mode)
  mov al, 0x11
  out 0x20, al
  out 0xa1, al
  mov al, 0x08
  out 0x21, al
  mov al, 0x70
  out 0xa1, al
  mov al, 0x04
  out 0x21, al
  mov al, 0x02
  out 0xa1, al
  mov al, 0x01
  out 0x21, al
  out 0xa1, al

  ; PIT channel 0: mode 2, divisor 0x0400 (~1.17 kHz) so several ticks fall
  ; inside the CLI spin below.
  mov al, 0x34
  out 0x43, al
  mov al, 0x00
  out 0x40, al
  mov al, 0x04
  out 0x40, al

  ; unmask IRQ0 only
  mov al, 0xfe
  out 0x21, al
  mov al, 0xff
  out 0xa1, al

  mov dword [TSS_LIN + 4], TSS_ESP0
  mov word [TSS_LIN + 8], SEL_DATA0
  mov word [TSS_LIN + 0x66], 0x68

  lgdt [gdtdesc]
  mov ax, SEL_DATA0
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  mov ss, ax
  mov esp, 0x4000
  jmp SEL_CODE0:.flush
.flush:
  mov edi, IDT_LIN + 6 * 8
  mov eax, ud_handler
  mov cx, SEL_CODE0
  xor dl, dl
  mov bl, 0x8e
  call mk_gate
  mov edi, IDT_LIN + 8 * 8
  mov eax, irq8_handler
  mov cx, SEL_CODE0
  xor dl, dl
  mov bl, 0x8e
  call mk_gate
  mov edi, IDT_LIN + 13 * 8
  mov eax, gp_handler
  mov cx, SEL_CODE0
  xor dl, dl
  mov bl, 0x8e
  call mk_gate
  mov edi, IDT_LIN + 0x21 * 8
  mov eax, int21_handler
  mov cx, SEL_CODE0
  xor dl, dl
  mov bl, 0xee
  call mk_gate
  lidt [idtdesc]
  mov ax, SEL_TSS
  ltr ax

  mov esi, v86_stub_start
  mov edi, V86_STUB
  mov ecx, v86_stub_end - v86_stub_start
  cld
  rep movsb

  push dword V86_SS
  push dword V86_SP
  push dword 0x00023202
  push dword 0
  push dword V86_STUB
  iretd

; ---- handlers ---------------------------------------------------------------
reload_flat:
  push ax
  mov ax, SEL_DATA0
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  pop ax
  ret

irq8_handler:
  mov al, 0x20                   ; EOI before the reflected handler runs:
  out 0x20, al                   ; the next IRQ must be able to nest
  call reload_flat
  mov [F8], esp
  mov ebp, esp
  ; No serial output here: printing waits on the UART THRE with IF=0, and a
  ; pending IRQ would then be delivered right after the IRET to V86, before
  ; the V86 handler's first instruction -- artificial nesting. Record the
  ; frame and print the table after the run instead.
  mov ecx, [IRQS]
  inc dword [IRQS]
  shl ecx, 5
  add ecx, IRQ_TAB
  mov eax, [ebp + 0x0c]
  mov [ecx + 0], eax
  mov eax, [ebp + 0x10]
  mov [ecx + 4], eax
  mov eax, [ebp + 0x04]
  mov [ecx + 8], eax
  mov eax, [ebp + 0x08]
  mov [ecx + 12], eax
  mov eax, [MARK + 0x10]
  mov [ecx + 16], eax

  ; reflect: V86 frame at [SS:ESP-6], enter the V86 IRQ handler
  mov eax, [ebp + 0x0c]
  and eax, 0xffff
  sub eax, 6
  mov ecx, [ebp + 0x10]
  and ecx, 0xffff
  shl ecx, 4
  add ecx, eax
  mov edx, [ebp + 0x00]
  mov [ecx], dx
  mov edx, [ebp + 0x04]
  mov [ecx + 2], dx
  mov edx, [ebp + 0x08]
  mov [ecx + 4], dx
  mov dword [ebp + 0x00], V86_HAND
  mov dword [ebp + 0x04], 0
  mov dword [ebp + 0x0c], eax
  mov esp, [F8]
  iretd

int21_handler:
  call reload_flat
  mov [F21], esp
  mov ebp, esp
  mov esi, s_f21
  call pframe
  mov esi, s_mark
  call puts
  mov esi, s_m0
  movzx eax, word [MARK + 0]
  call field
  mov esi, s_m1
  movzx eax, word [MARK + 2]
  call field
  mov esi, s_irqn
  mov eax, [IRQS]
  call field
  mov al, 10
  call putc
  ; print the recorded IRQ frames (frame count is deterministic if the PIT
  ; period is long relative to the reflection path)
  mov esi, s_irqtab
  call puts
  mov al, 10
  call putc
  mov ecx, [IRQS]
  cmp ecx, 6
  jbe .cap
  mov ecx, 6
.cap:
  xor ebx, ebx
.loop:
  cmp ebx, ecx
  jae .tabdone
  mov edi, ebx
  shl edi, 5
  add edi, IRQ_TAB
  mov esi, s_irq
  call puts
  mov esi, s_i
  mov eax, ebx
  call field
  mov esi, s_esp
  mov eax, [edi + 0]
  call field
  mov esi, s_ss
  mov eax, [edi + 4]
  call field
  mov esi, s_cs
  mov eax, [edi + 8]
  call field
  mov esi, s_fl
  mov eax, [edi + 12]
  call field
  mov esi, s_cnt
  mov eax, [edi + 16]
  call field
  mov al, 10
  call putc
  inc ebx
  jmp .loop
.tabdone:
  mov esi, s_done
  call puts
  mov al, 5
  jmp exit_port

ud_handler:
  call reload_flat
  mov [FGP], esp
  mov ebp, esp
  mov esi, s_ud
  call pframe
  jmp fail_exit

gp_handler:
  call reload_flat
  mov [FGP], esp
  mov ebp, esp
  mov esi, s_gp
  call puts
  mov esi, s_ec
  mov eax, [ebp + 0x00]
  call field
  mov esi, s_eip
  mov eax, [ebp + 0x04]
  call field
  mov esi, s_cs
  mov eax, [ebp + 0x08]
  call field
  mov esi, s_esp
  mov eax, [ebp + 0x10]
  call field
  mov esi, s_ss
  mov eax, [ebp + 0x14]
  call field
  mov al, 10
  call putc
fail_exit:
  mov al, 1
exit_port:
  mov dx, 0xf4
  out dx, al
.hang:
  hlt
  jmp .hang

; ---- output helpers ---------------------------------------------------------
putc:
  push eax
  push edx
  mov ah, al
.wait:
  mov dx, 0x3fd
  in al, dx
  test al, 0x20
  jz .wait
  mov dx, 0x3f8
  mov al, ah
  out dx, al
  pop edx
  pop eax
  ret

puts:
  push eax
  push esi
.next:
  mov al, [esi]
  test al, al
  jz .done
  call putc
  inc esi
  jmp .next
.done:
  pop esi
  pop eax
  ret

hex32:
  push eax
  push ebx
  push ecx
  mov ebx, eax
  mov ecx, 8
.loop:
  rol ebx, 4
  mov eax, ebx
  and eax, 0xf
  cmp eax, 10
  jb .digit
  add eax, 'a' - 10 - '0'
.digit:
  add eax, '0'
  call putc
  dec ecx
  jnz .loop
  pop ecx
  pop ebx
  pop eax
  ret

field:
  call puts
  call hex32
  mov al, ' '
  call putc
  ret

pframe:
  call puts
  mov esi, s_eip
  mov eax, [ebp + 0x00]
  call field
  mov esi, s_cs
  mov eax, [ebp + 0x04]
  call field
  mov esi, s_fl
  mov eax, [ebp + 0x08]
  call field
  mov esi, s_esp
  mov eax, [ebp + 0x0c]
  call field
  mov esi, s_ss
  mov eax, [ebp + 0x10]
  call field
  mov esi, s_es
  mov eax, [ebp + 0x14]
  call field
  mov esi, s_ds
  mov eax, [ebp + 0x18]
  call field
  mov esi, s_fs
  mov eax, [ebp + 0x1c]
  call field
  mov esi, s_gs
  mov eax, [ebp + 0x20]
  call field
  mov al, 10
  call putc
  ret

mk_gate:
  mov [edi], ax
  mov [edi + 2], cx
  mov [edi + 4], dl
  mov [edi + 5], bl
  shr eax, 16
  mov [edi + 6], ax
  ret

; ---- V86 stub ---------------------------------------------------------------
bits 16
v86_stub_start:
  mov ax, 0
  mov ds, ax
  mov es, ax
  mov ax, 0x3000
  mov fs, ax
  mov ax, 0x4000
  mov gs, ax
  mov ax, V86_SS
  mov ss, ax
  mov sp, V86_SP
  mov word [MARK + 0], 0xaaaa
  ; IF=0 must block IRQ0 for the whole spin (several PIT ticks); the first
  ; IRQ's recorded counter must be >= the spin count. An IRQ delivered while
  ; IF=0 would record a smaller counter -- the crash's critical-section
  ; violation.
  cli
  mov cx, 0xffff
.cli_spin:
  inc dword [MARK + 0x10]
  loop .cli_spin
  sti
.wait:
  inc dword [MARK + 0x10]        ; loop iterations since reset: the tick
  mov ax, [MARK + 2]             ; arrival measured on the instruction axis
  cmp ax, 3
  jb .wait
  cli
  mov word [MARK + 2], 0xbbbb
  mov ax, 0x3333
  int 0x21
  hlt
  jmp $
times (0x200 - ($ - v86_stub_start)) db 0x90
v86_irq_handler:
  inc word [MARK + 2]
  ; The ring-0 reflection path does not preserve the V86 general registers
  ; (EMM386's monitor saves and restores them itself); every wait re-reads
  ; the count from memory instead of keeping it in a register.
.spin:
  cmp word [MARK + 2], 3
  jb .spin
  iret
v86_stub_end:
bits 32

; ---- data -------------------------------------------------------------------
gdt:
  dq 0
  dq 0x00cf9b000000ffff
  dq 0x00cf93000000ffff
  dq 0x0000890024000067
gdt_end:

gdtdesc:
  dw gdt_end - gdt - 1
  dd gdt

idtdesc:
  dw 0x17f
  dd IDT_LIN

s_irq:  db "IRQ ", 0
s_irqtab: db "irqtab:", 0
s_i:    db "i=", 0
s_f21:  db "F21 ", 0
s_gp:   db "GP  ", 0
s_ud:   db "UD  ", 0
s_eip:  db "eip=", 0
s_cs:   db "cs=", 0
s_fl:   db "fl=", 0
s_cnt:  db "cnt=", 0
s_esp:  db "esp=", 0
s_ss:   db "ss=", 0
s_es:   db "es=", 0
s_ds:   db "ds=", 0
s_fs:   db "fs=", 0
s_gs:   db "gs=", 0
s_ec:   db "ec=", 0
s_irqn: db "irqs=", 0
s_mark: db "mark ", 0
s_m0:   db "m0=", 0
s_m1:   db "m1=", 0
s_done: db "v86-irq done", 10, 0
