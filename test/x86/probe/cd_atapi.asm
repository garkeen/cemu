; ATAPI smoke probe (not an acceptance test): drives the secondary IDE
; channel's packet device from real mode the way SeaBIOS's cdrom_boot does —
; IDENTIFY PACKET DEVICE, then a PACKET command carrying a READ(10) CDB for the
; El Torito boot record volume descriptor (LBA 0x11) — and prints what the
; device hands back on COM1. Run the same image on cemu and on
; qemu-system-i386 with the same CD-ROM: the transcripts must match byte for
; byte. That dual run is the reference this device model is judged against
; (AGENTS.md §五 参考优先; §六 手写探针只作冒烟探针).
;
; Build: nasm -f bin -o cd_atapi.bin cd_atapi.asm
; Run:   cemu --machine x86 --isa x86 -cdrom linux.iso cd_atapi.bin
;        qemu-system-i386 -drive file=cd_atapi.bin,format=raw,if=ide,index=0 \
;            -cdrom linux.iso -boot c -display none -serial stdio -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
  bits 16
  org 0x7c00

%define CMD   0x170           ; secondary channel task file
%define CTRL  0x376           ; secondary channel device control / alt status
%define IDBUF 0x8000          ; IDENTIFY PACKET DEVICE data lands here
%define RDBUF 0x8800          ; the 2048-byte CD block lands here

start:
  cli
  xor ax, ax
  mov ds, ax
  mov ss, ax
  mov sp, 0x7c00
  call uart_init
  mov si, m_boot
  call puts

  ; Software reset (SRST) — a packet device presents its ATAPI signature after
  ; one (ATA/ATAPI-7 §9.5) — then select device 0 and read the task file back.
  mov dx, CTRL
  mov al, 0x0e                ; SRST | nIEN | HD15
  out dx, al
  mov al, 0x0a                ; nIEN | HD15
  out dx, al
  mov dx, CMD + 6
  mov al, 0xa0
  out dx, al
  mov si, m_sig
  call puts
  mov bx, CMD + 2             ; sector count .. cylinder high
.sig:
  mov dx, bx
  in al, dx
  call puthex8
  mov al, ' '
  call putc
  inc bx
  cmp bx, CMD + 6
  jb .sig
  call crlf

  ; ---- IDENTIFY PACKET DEVICE (0xA1): 512 bytes of device identity ----
  call zero_tf
  mov dx, CMD + 7
  mov al, 0xa1
  out dx, al
  call wait_drq
  jc .idfail
  mov dx, CMD
  mov ax, 0x0800
  mov es, ax
  xor di, di
  mov cx, 256
  rep insw
  call crlf

  ; ---- PACKET (0xA0) with READ(10), LBA 0x11, one 2048-byte block ----
  mov dx, CMD + 6
  mov al, 0xa0
  out dx, al
  call zero_tf
  mov dx, CMD + 4
  mov al, 0x00                ; byte count limit = 2048 (low)
  out dx, al
  mov dx, CMD + 5
  mov al, 0x08                ; byte count limit (high)
  out dx, al
  mov dx, CMD + 7
  mov al, 0xa0
  out dx, al
  call wait_drq
  jc .rdfail
  mov dx, CMD                 ; the 12-byte CDB, as outsw moves its words
  mov ax, 0x0028              ; 28 00  READ(10)
  out dx, ax
  mov ax, 0x0000              ; 00 00
  out dx, ax
  mov ax, 0x1100              ; 00 11  -> LBA 0x11 (big-endian field)
  out dx, ax
  mov ax, 0x0000              ; 00 00
  out dx, ax
  mov ax, 0x0001              ; 01 00  -> one block
  out dx, ax
  mov ax, 0x0000              ; 00 00  control
  out dx, ax
  call wait_drq
  jc .rdfail
  mov dx, CMD
  mov ax, 0x0800
  mov es, ax
  mov di, RDBUF - 0x8000
  mov cx, 1024
  rep insw
  mov si, m_st
  call puts
  mov dx, CMD + 7
  in al, dx
  call puthex8
  call crlf
  mov si, m_blk
  call puts
  mov si, RDBUF
  mov cx, 32
  call puthexblock
  call crlf
  jmp .exit

.idfail:
  mov si, m_idfail
  call puts
  jmp .exit
.rdfail:
  mov si, m_rdfail
  call puts

.exit:
  mov al, 5                   ; emulator status = (5 << 1) | 1 = 11
  mov dx, 0xf4
  out dx, al
.hang:
  hlt
  jmp .hang

; ---- helpers ---------------------------------------------------------------

zero_tf:                      ; feature/count/number/cylinder low/high = 0
  mov dx, CMD + 1
  xor al, al
  mov cx, 5
.loop:
  out dx, al
  inc dx
  loop .loop
  ret

; Poll status until DRQ is set (BSY clear); carry set = ERR or timeout.
wait_drq:
  mov cx, 0xffff
.poll:
  mov dx, CMD + 7
  in al, dx
  test al, 0x80               ; BSY: still working
  jnz .next
  test al, 0x08               ; DRQ: data ready
  jnz .ok
  test al, 0x01               ; ERR: the device refused
  jnz .fail
.next:
  loop .poll
  mov si, m_to
  call puts
.fail:
  stc
  ret
.ok:
  clc
  ret

puthex8:                      ; AL
  push ax
  shr al, 4
  call puthexdig
  pop ax
  call puthexdig
  ret
puthexdig:                    ; AL low nibble
  and al, 0x0f
  cmp al, 10
  jb .digit
  add al, 'a' - 10
  jmp putc
.digit:
  add al, '0'
  jmp putc

puthexblock:                  ; DS:SI, CX bytes, space separated
  test cx, cx
  jz .done
.loop:
  lodsb
  call puthex8
  mov al, ' '
  call putc
  loop .loop
.done:
  ret

puts:                         ; SI = NUL-terminated string
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

putc:                         ; AL = character, waits for THRE
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

m_boot:   db "cd-atapi", 13, 10, 0
m_sig:    db "sig=", 0
m_st:     db "st=", 0
m_blk:    db "blk=", 0
m_to:     db "to", 13, 10, 0
m_idfail: db "noid", 13, 10, 0
m_rdfail: db "nord", 13, 10, 0

  times 510-($-$$) db 0
  dw 0xaa55
