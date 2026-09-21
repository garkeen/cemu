; IRQ0 delivery while spinning smoke probe (not an acceptance test).
;
; The 8254 channel 0 edge is the PC's periodic timer interrupt; a guest that
; busy-waits on a tick (Linux's calibrate_delay_converge() spins on jiffies,
; with interrupts enabled and no hlt) only makes progress if the emulator
; raises IRQ0 from wall time and the 8259 delivers it. An emulator that only
; advances the timer while the CPU is asleep (hlt) leaves such a guest
; spinning forever, which is exactly how the linux.iso boot stalled at
; "Calibrating delay loop...".
;
; The probe does what the kernel does: builds an IDT, remaps the 8259 pair,
; programs channel 0 for 100 Hz, unmasks IRQ0 only, sets IF, and then SPINS
; (deliberately no hlt) counting the ticks its ISR receives. It reports the
; count on COM1 and exits via debug-exit.
;
; Build: nasm -f bin -o pit_irq.bin pit_irq.asm
; Run:   cemu --machine x86 --isa x86 pit_irq.bin
;        qemu-system-i386 -drive file=pit_irq.bin,format=raw,if=ide,index=0 \
;            -boot c -display none -serial stdio -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit value 5 when at least one tick arrived, 1 otherwise.

  bits 16
  org 0x7c00

%define DBGEXIT  0xf4
%define SPIN     0x400000      ; spin budget: ~20M instructions

start:
  cli
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7c00
  call uart_init
  mov si, m_boot
  call puts

  ; IDT at 0x8000: every vector to a bare iret, vector 0x20 to the ISR.
  mov di, 0x8000
  mov cx, 256
  xor bx, bx
  mov ax, isr_default
.fill:
  mov [di], ax
  mov [di + 2], bx
  add di, 4
  loop .fill
  mov di, 0x8000 + 0x20 * 4
  mov ax, timer_isr
  mov [di], ax
  mov [di + 2], bx
  mov word [idt_limit], 0x3ff
  mov dword [idt_base], 0x8000
  lidt [idt_limit]

  ; 8259 pair: remap to 0x20/0x28, level-triggered 8086 mode, unmask IRQ0 only.
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
  mov al, 0xfe
  out 0x21, al                  ; master: only IRQ0 open
  mov al, 0xff
  out 0xa1, al                  ; slave: all masked

  ; 8254 channel 0: mode 3 (square wave), divisor for ~100 Hz.
  mov al, 0x36
  out 0x43, al
  mov ax, 11932
  out 0x40, al
  mov al, ah
  out 0x40, al

  mov dword [ticks], 0
  sti
  mov ecx, SPIN
.spin:
  mov eax, [ticks]
  cmp eax, 10
  jae .done
  dec ecx
  jnz .spin
.done:
  cli
  mov si, m_ticks
  call puts
  mov eax, [ticks]
  call puthex32
  call crlf
  mov eax, [ticks]
  test eax, eax
  jz .fail
  mov si, m_ok
  call puts
  mov al, 5
  jmp exit
.fail:
  mov si, m_fail
  call puts
  mov al, 1
exit:
  mov dx, DBGEXIT
  out dx, al
  hlt
  jmp $

timer_isr:
  pusha
  inc dword [ticks]
  mov al, 0x20
  out 0x20, al                  ; non-specific EOI
  popa
  iret

isr_default:
  iret

puthex32:                       ; EAX
  push eax
  shr eax, 16
  call puthex16
  pop eax
  call puthex16
  ret

puthex16:                       ; AX
  push ax
  xchg al, ah
  call puthex8
  pop ax
  call puthex8
  ret

puthex8:                        ; AL
  push ax
  shr al, 4
  call puthexdig
  pop ax
  call puthexdig
  ret

puthexdig:
  and al, 0x0f
  cmp al, 10
  jb .digit
  add al, 'a' - 10
  jmp putc
.digit:
  add al, '0'
  jmp putc

puts:
  lodsb
  test al, al
  jz .done
  call putc
  jmp puts
.done:
  ret

crlf:
  mov al, 13
  call putc
  mov al, 10
  jmp putc

putc:
  push ax
  mov dx, 0x3fd
.wait:
  in al, dx
  test al, 0x20
  jz .wait
  pop ax
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

idt_limit: dw 0
idt_base:  dd 0
ticks:     dd 0
m_boot:    db "pit-irq: ", 0
m_ticks:   db "ticks=", 0
m_ok:      db "pit-irq ok", 13, 10, 0
m_fail:    db "pit-irq FAIL (no IRQ0)", 13, 10, 0

  times 510-($-$$) db 0
  dw 0xaa55
