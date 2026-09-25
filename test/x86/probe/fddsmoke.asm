; SeaBIOS floppy read/write smoke probe (not an acceptance test).
;
; test/x86/run.sh places this sector in a generated 1.44 MiB image and boots
; it from A:.  The probe uses only the PC BIOS INT 13h path:
;
;   fill 512 bytes -> write CHS 0/0/2 -> read CHS 0/0/2 -> compare
;
; SeaBIOS owns the 8272A command sequences and 8237 channel-2 programming, so
; this probe checks the complete firmware-to-medium data path without embedding
; a second, hand-written DMA programming model in the test.  It needs no video
; BIOS, operating system, or GUI.  COM1 and port 0xF4 provide the result.

bits 16
org 0x7c00

start:
  cli
  cld
  xor ax, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7c00
  mov [boot_drive], dl
  call uart_init

  ; Deterministic 512-byte payload at 0000:8200.  This is a real-mode
  ; boot sector, so LOOP has its original 16-bit counter semantics.
  mov di, 0x8200
  mov cx, 512
  mov al, 0x5a
.fill:
  mov [di], al
  add al, 17
  inc di
  loop .fill

  ; INT 13h AH=03, one sector, C=0/H=0/S=2, ES:BX=0000:8200.  Three bounded
  ; attempts cover a controller reset race without allowing a hang.
  mov byte [retries], 3
.write_retry:
  mov bx, 0x8200
  mov cx, 0x0002
  xor dh, dh
  mov dl, [boot_drive]
  mov ax, 0x0301
  int 0x13
  jnc .wrote
  dec byte [retries]
  jnz .write_retry
  jmp write_failed

.wrote:
  ; Read the just-written sector into a distinct buffer at 0000:8400.
  mov byte [retries], 3
.read_retry:
  mov bx, 0x8400
  mov cx, 0x0002
  xor dh, dh
  mov dl, [boot_drive]
  mov ax, 0x0201
  int 0x13
  jnc .read_done
  dec byte [retries]
  jnz .read_retry
  jmp read_failed

.read_done:
  mov si, 0x8200
  mov di, 0x8400
  mov cx, 512
.compare:
  mov al, [si]
  cmp al, [di]
  jne compare_failed
  inc si
  inc di
  loop .compare

  mov si, m_ok
  call puts
  mov al, 5
  jmp exit

write_failed:
  mov si, m_write_failed
  jmp fail
read_failed:
  mov si, m_read_failed
  jmp fail
compare_failed:
  mov si, m_compare_failed
fail:
  call puts
  mov al, 2
  jmp exit

puts:
  lodsb
  test al, al
  jz .done
  call putc
  jmp puts
.done:
  ret

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

exit:
  mov dx, 0xf4
  out dx, al
  hlt
  jmp $

boot_drive: db 0
retries:    db 0
m_ok:            db 'floppy ok', 13, 10, 0
m_write_failed:  db 'floppy FAIL: INT 13h write', 13, 10, 0
m_read_failed:   db 'floppy FAIL: INT 13h read', 13, 10, 0
m_compare_failed:
                 db 'floppy FAIL: sector compare', 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xaa55
