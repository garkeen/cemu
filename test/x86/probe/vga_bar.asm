; The VGA function's PCI BAR types and sizes, read back through the sizing
; protocol (a self-contained boot sector; no firmware, no option ROM).
;
; The card is QEMU's standard VGA identity 1234:1111 at 00:02.0:
;
;   Region 0 is a 32-bit prefetchable memory BAR, 16MB (vga.h kVgaVramSize;
;   QEMU registers it with PCI_BASE_ADDRESS_MEM_PREFETCH only, so the sizing
;   read-back ends in ...008, not ...00c).
;   Region 2 is a 4KB *MMIO* BAR, not an I/O BAR (QEMU standard-VGA
;   specification). A sizing read-back of an I/O BAR would end in ...001; this
;   probe requires ...000 and so catches that regression.
;
; The window itself is not exercised: a real-mode guest cannot reach the high
; MMIO address firmware assigns, and on QEMU this probe runs under SeaBIOS,
; which has already placed the BARs. The mapping/remove-on-write path is covered
; by VgaBarWritten/VgaBar2Written and the reset wiring; this cell pins the
; identity that a guest can inspect.
;
; Build: nasm -f bin -o vga_bar.bin vga_bar.asm
; Run:   cemu --machine x86 --isa x86 vga_bar.bin
;        qemu-system-i386 -drive file=vga_bar.bin,format=raw,if=ide,index=0 \
;            -boot c -vga std -display none -serial file:... -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit value 5 on pass (status 11), 1 on fail. A failure names its
; step.

  bits 16
  org 0x7c00

%define DBGEXIT 0xf4
%define CFG 0x80001000          ; enable, bus 0, device 2, function 0, offset 0

start:
  cli
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7c00
  call uart_init

  ; Region 2: a 4KB memory BAR, type bits 00.
  mov byte [step_no], 1
  mov eax, CFG + 0x18
  call pci_sel
  mov eax, 0xffffffff
  call pci_write
  call pci_read
  cmp eax, 0xfffff000
  jne fail

  ; Region 0: 16MB, 32-bit prefetchable (type bits 1000).
  mov byte [step_no], 2
  mov eax, CFG + 0x10
  call pci_sel
  mov eax, 0xffffffff
  call pci_write
  call pci_read
  cmp eax, 0xff000008
  jne fail

  ; Leave the BAR at 0: no decode. The probe exits immediately, but a sizing
  ; write restored to 0 is the reset state the next consumer expects.
  mov eax, CFG + 0x10
  call pci_sel
  xor eax, eax
  call pci_write
  mov eax, CFG + 0x18
  call pci_sel
  xor eax, eax
  call pci_write

  mov si, m_pass
  call puts
  mov dx, DBGEXIT
  mov ax, 5
  out dx, ax
  jmp $

fail:
  mov si, m_fail
  call puts
  mov al, [step_no]
  add al, '0'
  call putc
  mov dx, DBGEXIT
  mov ax, 1
  out dx, ax
  jmp $

pci_sel:                      ; EAX = 0x80000000 | bus/dev/fn | offset
  mov dx, 0xcf8
  out dx, eax
  ret

pci_read:                     ; -> EAX
  mov dx, 0xcfc
  in eax, dx
  ret

pci_write:                    ; EAX = value
  mov dx, 0xcfc
  out dx, eax
  ret

puts:                         ; SI = NUL-terminated string
  lodsb
  test al, al
  jz .done
  call putc
  jmp puts
.done:
  ret

putc:                         ; AL = character, waits for THRE
  push ax
  push si
  mov dx, 0x3fd
.wait:
  in al, dx
  test al, 0x20
  jz .wait
  pop si
  pop ax
  mov dx, 0x3f8
  out dx, al
  ret

uart_init:
  mov dx, 0x3fb               ; LCR: divisor latch
  mov al, 0x80
  out dx, al
  mov dx, 0x3f8               ; DLL = 1 (115200 baud)
  mov al, 0x01
  out dx, al
  mov dx, 0x3f9               ; DLH = 0
  xor al, al
  out dx, al
  mov dx, 0x3fb               ; 8N1, DLAB = 0
  mov al, 0x03
  out dx, al
  mov dx, 0x3f9               ; IER = 0
  xor al, al
  out dx, al
  ret

step_no:  db 0
m_pass:   db "vga-bar ok", 13, 10, 0
m_fail:   db "vga-bar FAIL step=", 0

  times 510-($-$$) db 0
  dw 0xaa55
