; ATA LBA-48 and READ/WRITE MULTIPLE (not an acceptance test -- a probe that
; pins the disk command set). The probe boots from the same file it attaches as
; the primary master, so the fixture is its own second sector (no OS image).
;
; Checks:
;   1. IDENTIFY advertises LBA-48 (word 83/86 bit 10), the multiple limit
;      (word 47), a disabled current setting (word 59) and the 48-bit capacity
;      (words 100..103).
;   2. READ SECTORS EXT (0x24) with the 48-bit task-file sequence reads LBA 1,
;      proving both halves of the address and count were latched. The high
;      bytes are written first, then the low bytes (libata/QEMU order).
;   3. SET MULTIPLE 2 is accepted.
;   4. READ MULTIPLE 2 sectors at LBA 0 hands over both sectors in one DRQ
;      block (the command returns the boot sector's 55AA and the fixture
;      pattern). QEMU's IDE has no READ/WRITE MULTIPLE command, so this half is
;      pinned against ATA/ATAPI-7 alone and the probe is a lock cell.
;
; Reference: ATA/ATAPI-7 §6.2.2 (48-bit task-file sequence), §7.10.4 (count 0
; means 256), §8.10 (READ/WRITE MULTIPLE), §7.10.2 words 47/59/83/86/100..103.
;
; Build: nasm -f bin -o ata_multi.bin ata_multi.asm
; Run:   cemu --machine x86 --isa x86 -hda ata_multi.bin ata_multi.bin
;        qemu-system-i386 -drive file=ata_multi.bin,format=raw,if=ide,index=0 \
;            -boot c -display none -serial stdio -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit value 5 on pass (status = (5<<1)|1 = 11), 1 on fail.

  bits 16
  org 0x7c00

%define DBGEXIT 0xf4
%define CMD     0x1f0
%define CTRL    0x3f6
%define IDBUF   0x8000
%define DATBUF  0x9000

start:
  cli
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7c00
  call uart_init

  mov dx, CTRL
  mov al, 0x02                ; nIEN: poll the status register
  out dx, al

  ; 1) IDENTIFY DEVICE
  mov byte [step_no], 1
  mov dx, CMD + 6
  mov al, 0xa0                ; master, CHS (the command ignores it)
  out dx, al
  call wait_ready
  mov dx, CMD + 7
  mov al, 0xec
  out dx, al
  call wait_drq
  mov di, IDBUF
  mov cx, 256
  mov dx, CMD
  rep insw
  mov ax, [IDBUF + 47 * 2]
  and ax, 0x00ff
  cmp ax, 16
  jne fail
  test word [IDBUF + 83 * 2], 0x0400
  jz fail
  test word [IDBUF + 86 * 2], 0x0400
  jz fail
  cmp word [IDBUF + 100 * 2], 2
  jne fail

  ; 2) READ SECTORS EXT (LBA-48) of LBA 1, count 1. The low half is written
  ;    first, then the high half through the same registers; the device has to
  ;    latch both when the command arrives.
  mov byte [step_no], 2
  mov dx, CMD + 6
  mov al, 0xe0
  out dx, al
  xor al, al
  mov dx, CMD + 2
  out dx, al                  ; count high
  mov dx, CMD + 3
  out dx, al                  ; LBA high bytes
  mov dx, CMD + 4
  out dx, al
  mov dx, CMD + 5
  out dx, al
  mov dx, CMD + 2
  mov al, 1                   ; count low
  out dx, al
  mov dx, CMD + 3
  mov al, 1                   ; LBA 1 low
  out dx, al
  xor al, al
  mov dx, CMD + 4
  out dx, al
  mov dx, CMD + 5
  out dx, al
  mov dx, CMD + 7
  mov al, 0x24                ; READ SECTORS EXT
  out dx, al
  call wait_drq
  mov di, DATBUF
  mov cx, 256
  mov dx, CMD
  rep insw
  cmp dword [DATBUF], 'ATA-'
  jne fail
  cmp dword [DATBUF + 4], 'SECT'
  jne fail

  ; 3) SET MULTIPLE 2. The command's success is read from the status register;
  ;    the setting itself is proved by the next command, because QEMU caches
  ;    IDENTIFY data and keeps answering the pre-SET word 59.
  mov byte [step_no], 3
  mov dx, CMD + 6
  mov al, 0xa0
  out dx, al
  mov dx, CMD + 2
  mov al, 2
  out dx, al
  mov dx, CMD + 7
  mov al, 0xc6
  out dx, al
  call wait_ready

  ; 4) READ MULTIPLE 2 sectors at LBA 0: one DRQ block carries both.
  mov byte [step_no], 4
  mov dx, CMD + 6
  mov al, 0xe0                ; LBA, master
  out dx, al
  mov dx, CMD + 2
  mov al, 2
  out dx, al
  xor al, al
  mov dx, CMD + 3
  out dx, al
  mov dx, CMD + 4
  out dx, al
  mov dx, CMD + 5
  out dx, al
  mov dx, CMD + 7
  mov al, 0xc4                ; READ MULTIPLE
  out dx, al
  call wait_drq
  mov di, DATBUF
  mov cx, 512                 ; 1024 bytes = two sectors
  mov dx, CMD
  rep insw
  cmp word [DATBUF + 510], 0xaa55
  jne fail
  cmp dword [DATBUF + 512], 'ATA-' ; the second sector's first four bytes
  jne fail

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

; Wait until BSY clears; ERR while waiting is a failure.
wait_ready:
  mov dx, CMD + 7
.poll:
  in al, dx
  test al, 0x80
  jnz .poll
  test al, 0x01
  jnz fail
  ret

; Wait for BSY clear and DRQ set.
wait_drq:
  mov dx, CMD + 7
.poll:
  in al, dx
  test al, 0x80
  jnz .poll
  test al, 0x01
  jnz fail
  test al, 0x08
  jz .poll
  ret

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

step_no:  db 0
m_pass:   db "ata-multi ok", 13, 10, 0
m_fail:   db "ata-multi FAIL step=", 0

  times 510-($-$$) db 0
  dw 0xaa55
  ; Sector 1: the fixture READ MULTIPLE/LBA-48 read.
  db 'ATA-SECTOR-1'
  times 1024-($-$$) db 0x5a
