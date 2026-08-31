; Boot-sector smoke probe for the x86_min machine (not an acceptance test:
; acceptance is kvm-unit-tests realmode). Loaded at 0x7C00 with CS:IP =
; 0000:7C00 and DL=0x80, initializes COM1, prints a marker line, then exits
; through the QEMU isa-debug-exit port 0xF4 with payload 5 (emulator status
; = (5 << 1) | 1 = 11).
;
; Build: nasm -f bin -o x86_smoke.bin x86_smoke.asm
; Run:   cemu --machine x86 --isa x86 x86_smoke.bin
;        qemu-system-i386 -drive file=x86_smoke.bin,format=raw,if=ide \
;            -display none -serial file:smoke_qemu.txt -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Both must print "cemu-x86-smoke" and exit with status 11.
  bits 16
  org 0x7c00

start:
  cli
  xor ax, ax
  mov ds, ax
  mov ss, ax
  mov sp, 0x7c00

  ; COM1 init: divisor 1 (115200 baud), 8N1, interrupts off
  mov dx, 0x3fb          ; LCR: set DLAB
  mov al, 0x80
  out dx, al
  mov dx, 0x3f8          ; DLL
  mov al, 0x01
  out dx, al
  mov dx, 0x3f9          ; DLH
  xor al, al
  out dx, al
  mov dx, 0x3fb          ; LCR: 8N1, DLAB=0
  mov al, 0x03
  out dx, al
  mov dx, 0x3f9          ; IER
  xor al, al
  out dx, al

  mov si, msg
.next:
  lodsb
  test al, al
  jz .done
  call putc
  jmp .next

.done:
  mov al, 5              ; exit payload: emulator status = 5 + 1
  mov dx, 0xf4
  out dx, al
.hang:
  hlt
  jmp .hang

putc:                    ; AL = character, waits for THRE
  push ax
  mov dx, 0x3fd          ; LSR
.wait:
  in al, dx
  test al, 0x20
  jz .wait
  pop ax
  mov dx, 0x3f8          ; THR
  out dx, al
  ret

msg: db "cemu-x86-smoke", 13, 10, 0

  times 510-($-$$) db 0
  dw 0xaa55
