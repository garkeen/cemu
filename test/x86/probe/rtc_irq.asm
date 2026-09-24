; CMOS RTC periodic + alarm interrupt delivery (not an acceptance test).
;
; The MC146818 raises IRQ8 from three status-C flags: PF (once per the period
; status A's RS field selects), AF (the alarm registers match the clock) and UF
; (the update cycle ends, once a second). The line is a level, and a read of
; status C clears the flags — which is what drops it. So an emulator that never
; sets the flags delivers nothing, and one that never lowers the line delivers
; exactly one interrupt and then stops (IRQ8 is edge-triggered in the PC/AT
; wiring, so no further edges come until the line goes low again).
;
; The probe programs the pair the way a driver does: status A RS=6 (1024 Hz),
; status B = PIE|AIE|24h, and an all-don't-care alarm (0xc0 in all three alarm
; registers) — which the chip reads as "matches every value", so AF fires once
; a second with no BCD arithmetic needed. Then it HALTS: the periodic rate
; keeps the ISR running while the CPU sleeps, and the alarm takes a real
; second. Halting rather than spinning is deliberate — an instruction budget
; cannot bound a 1 Hz event across emulators of different speed (the same
; budget is ~1s of wall time on cemu and ~30ms on QEMU).
;
; It reports only whether each flag was seen, never a tick count: the count
; depends on host timing and the dual run compares transcripts.
;
; Build: nasm -f bin -o rtc_irq.bin rtc_irq.asm
; Run:   cemu --machine x86 --isa x86 rtc_irq.bin
;        qemu-system-i386 -drive file=rtc_irq.bin,format=raw,if=ide,index=0 \
;            -boot c -display none -serial stdio -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit value 5 when both flags arrived, 1 otherwise.

  bits 16
  org 0x7c00

%define DBGEXIT 0xf4
%define WAIT    2000          ; hlt iterations — the loop leaves early once AF
                              ; arrives, so this bound only bites on failure;
                              ; one hlt is ~16ms on cemu but ~1ms on QEMU

start:
  cli
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7c00
  call uart_init

  ; IDT at 0x8000: every vector to a bare iret, vector 0x28 to the ISR (after
  ; the remap the slave's line 0 — IRQ8 — is vector 0x28 + 0).
  mov di, 0x8000
  mov cx, 256
  xor bx, bx
  mov ax, isr_default
.fill:
  mov [di], ax
  mov [di + 2], bx
  add di, 4
  loop .fill
  mov di, 0x8000 + 0x28 * 4
  mov ax, rtc_isr
  mov [di], ax
  mov [di + 2], bx
  mov word [idt_limit], 0x3ff
  mov dword [idt_base], 0x8000
  lidt [idt_limit]

  ; 8259 pair -> 0x20/0x28, then open only the cascade (IRQ2) and the slave's
  ; line 0 (IRQ8). IRQ0 stays masked: nothing programs the PIT here, and its
  ; default iret stub would leave the master's ISR bit set.
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
  mov al, 0xfb
  out 0x21, al                  ; master: IRQ2 (the cascade) only
  mov al, 0xfe
  out 0xa1, al                  ; slave: IRQ8 only

  ; The RTC: status A RS=6 (1024 Hz), status B = PIE|AIE|24h, alarm registers
  ; all 0xc0 (don't care, so AF fires at every second boundary).
  mov al, 0x0a
  out 0x70, al
  mov al, 0x26
  out 0x71, al
  mov al, 0x0b
  out 0x70, al
  mov al, 0x62
  out 0x71, al
  mov al, 0x01
  out 0x70, al
  mov al, 0xc0
  out 0x71, al
  mov al, 0x03
  out 0x70, al
  mov al, 0xc0
  out 0x71, al
  mov al, 0x05
  out 0x70, al
  mov al, 0xc0
  out 0x71, al

  ; Read back status B and the seconds alarm: the two values the alarm path
  ; depends on, so a failure says which half broke.
  mov si, m_rb
  call puts
  mov al, 0x0b
  out 0x70, al
  in al, 0x71
  call puthex8
  mov si, m_a1
  call puts
  mov al, 0x01
  out 0x70, al
  in al, 0x71
  call puthex8
  call crlf

  mov byte [pf_seen], 0
  mov byte [af_seen], 0
  mov byte [uf_seen], 0
  sti
  mov ecx, WAIT
.wait:
  cmp byte [af_seen], 0
  jne .done
  hlt
  dec ecx
  jnz .wait
.done:
  cli
  mov si, m_pf
  call puts
  mov al, [pf_seen]
  call puthex8
  mov si, m_af
  call puts
  mov al, [af_seen]
  call puthex8
  mov si, m_uf
  call puts
  mov al, [uf_seen]
  call puthex8
  call crlf
  cmp byte [pf_seen], 0
  je .fail
  cmp byte [af_seen], 0
  je .fail
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

rtc_isr:
  pusha
  mov al, 0x0c                  ; status C: returns the flags and clears them,
  out 0x70, al                  ; which is what drops the IRQ8 line
  in al, 0x71
  test al, 0x40
  jz .no_pf
  mov byte [pf_seen], 1
.no_pf:
  test al, 0x10
  jz .no_uf
  mov byte [uf_seen], 1
.no_uf:
  test al, 0x20
  jz .no_af
  mov byte [af_seen], 1
.no_af:
  mov al, 0x20                  ; EOI the slave ...
  out 0xa0, al
  out 0x20, al                  ; ... and the master
  popa
  iret

isr_default:
  iret

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
pf_seen:   db 0
af_seen:   db 0
uf_seen:   db 0
m_rb:      db "b=", 0
m_a1:      db " a1=", 0
m_pf:      db "pf=", 0
m_af:      db " af=", 0
m_uf:      db " uf=", 0
m_ok:      db "rtc-irq ok", 13, 10, 0
m_fail:    db "rtc-irq FAIL (no RTC IRQ8)", 13, 10, 0

  times 510-($-$$) db 0
  dw 0xaa55
