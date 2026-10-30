; PIIX4 SMBus host controller (not an acceptance test -- a probe that pins one
; chipset function).
;
; The controller's window is named by configuration offset 0x90 (SMB_BASE, here
; 0xb100 with the I/O indicator) and gated by bit 0 of offset 0xD2, both already
; set when the guest starts; the bus underneath carries the eight SPD EEPROMs
; QEMU's pc machine attaches at 0x50-0x57. The probe walks the register file the
; way a guest does, printing one field per step:
;   cfg   SMB_BASE written into 00:01.3's configuration space and read back
;         (which also pins that a written base moves the decode), then the gate
;         byte that turns the window on
;   st/ctl/aux  the image the machine starts with: no status bit, HST_CNT
;         holding nothing at all (its block-mode preset lives in HST_AUXCTL),
;         HST_AUXCTL holding that preset
;   wq    a byte-data write to SPD 0x50 (0xAB into its offset 0x02) with INTREN
;         clear: the first status read reports HOST_BUSY and the transaction
;         runs on it, so the second reports INTR
;   rd    that byte read back (HST_ADD bit 0 set) -- the EEPROM's offset
;         protocol: the write phase names the offset, the read phase hands out
;         from there
;   na    a byte-data read of an address nobody answers: DEV_ERR
;   im    the same write with INTREN set: the transaction has already run when
;         the status is first read
;   wd    a word-data read (low byte first) of the two bytes written before it
;   bl    a block read in HST_AUXCTL's block mode: the block's first byte on the
;         wire is its own length, which lands in HST_DAT0
;   bw    a block write byte by byte (block mode cleared): one BYTE_DONE per
;         byte cleared, INTR and HOST_BUSY cleared on the last
;   wr    the two bytes that block write left in the EEPROM
;   ib    an I2C block read: the command goes out in HST_DAT1, the first byte
;         lands in HST_BLKDAT and LAST_BYTE ends the transfer
;
; Read protocols take HST_ADD's direction bit set (0xA1 for 0x50); a write
; phase's bytes are the command plus its data, which is what the EEPROMs on this
; bus act on.
;
; Reference: PIIX4 datasheet §5 (SMBus host controller), SMBus 2.0 §4-5
; (protocols), hw/acpi/piix4.c (SMB_BASE at 0x90, gate byte at 0xD2) and QEMU
; hw/i2c/{pm_smbus,core,smbus_master,smbus_slave,smbus_eeprom}.c.
;
; Build: nasm -f elf32 -o smbus.o smbus.asm
;        ld.lld -m elf_i386 -T vbe_mode.ld -o smbus.elf smbus.o
; Run:   cemu --machine x86 --isa x86 smbus.elf
;        qemu-system-i386 -machine pc -m 32 -kernel smbus.elf -display none \
;            -no-reboot -device isa-debug-exit,iobase=0xf4,iosize=0x4 \
;            -serial file:smbus_qemu.txt
; Exit: debug-exit 5 (status 11) once the transcript is out.

  bits 32

%define CFG_ADDR 0xcf8
%define CFG_DATA 0xcfc
%define SMB      0xb100
%define HST_STS  SMB + 0x00
%define HST_CNT  SMB + 0x02
%define HST_CMD  SMB + 0x03
%define HST_ADD  SMB + 0x04
%define HST_DAT0 SMB + 0x05
%define HST_DAT1 SMB + 0x06
%define HST_BLKD SMB + 0x07
%define HST_AUX  SMB + 0x0d
%define SPD_WR   0xa0              ; 0x50 shifted left, direction bit clear
%define SPD_RD   0xa1              ; ... and set
%define DBGEXIT  0xf4

; One transaction's address, command and control byte: the HST_CNT write is what
; starts it (bit 6), and bits 4:2 select the protocol.
%macro XFER 3
  mov dx, HST_ADD
  mov al, %1
  out dx, al
  mov dx, HST_CMD
  mov al, %2
  out dx, al
  mov dx, HST_CNT
  mov al, %3
  out dx, al
%endmacro

%macro P 1
  mov esi, %1
  call puts
%endmacro

; Read a register and print it as two hex digits.
%macro RD 1
  mov dx, %1
  in al, dx
  call hex8
%endmacro

; Read HST_STS without printing it: with INTREN clear this is what runs the
; transaction the start left pending.
%macro TRIG 0
  mov dx, HST_STS
  in al, dx
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
  ; SMB_BASE = 0xb100 with the I/O indicator, and the gate byte that turns the
  ; window on. The probe names the base itself because a booted machine's
  ; firmware does: SeaBIOS's piix4_pm_config_setup (src/fw/pciinit.c) programs
  ; acpi_pm_base + 0x100, i.e. 0x700, so a value read back here says more about
  ; who ran last than about the device. Writing it also pins that a written base
  ; moves the decode, which is how the SMBus is found after that firmware pass.
  P m_cfg
  mov eax, 0x80000b90          ; bus 0, dev 1, func 3, offset 0x90
  mov dx, CFG_ADDR
  out dx, eax
  mov eax, 0xb101              ; 0xb100 | I/O-space indicator
  mov dx, CFG_DATA
  out dx, eax
  mov dx, CFG_DATA
  in eax, dx
  call hex32
  ; The gate byte lives at 0xD2, the third byte of the register at 0xD0.
  P m_d2
  mov eax, 0x80000bd0
  mov dx, CFG_ADDR
  out dx, eax
  mov dx, CFG_DATA + 2
  in al, dx
  call hex8

  ; The image the window answers with before any of it is used.
  P m_st
  RD HST_STS
  P m_ctl
  RD HST_CNT
  P m_aux
  RD HST_AUX

  ; Byte-data write with INTREN clear: deferred until the status is read.
  call clear_sts
  mov dx, HST_DAT0
  mov al, 0xab
  out dx, al
  P m_wq
  XFER SPD_WR, 0x02, 0x48       ; byte-data | START
  RD HST_STS                    ; HOST_BUSY: the transaction is still pending
  P m_comma
  RD HST_STS                    ; it ran on that read: INTR

  ; The same byte back, which is the EEPROM's offset protocol.
  call clear_sts
  P m_rd
  XFER SPD_RD, 0x02, 0x48
  TRIG
  RD HST_DAT0                   ; the byte the write left there

  ; An address no device answers.
  call clear_sts
  P m_na
  XFER 0x61, 0x00, 0x48
  RD HST_STS                    ; HOST_BUSY
  P m_comma
  RD HST_STS                    ; the bus reports DEV_ERR

  ; With INTREN the transaction has already run when the status is read.
  call clear_sts
  P m_im
  mov dx, HST_DAT0
  mov al, 0xab
  out dx, al
  XFER SPD_WR, 0x02, 0x49       ; byte-data | START | INTREN
  RD HST_STS

  ; Two bytes into offsets 0 and 1, then a word read of both.
  call clear_sts
  mov dx, HST_DAT0
  mov al, 0x12
  out dx, al
  XFER SPD_WR, 0x00, 0x49
  call clear_sts
  mov dx, HST_DAT0
  mov al, 0x34
  out dx, al
  XFER SPD_WR, 0x01, 0x49
  call clear_sts
  P m_wd
  XFER SPD_RD, 0x00, 0x4c       ; word-data | START
  TRIG
  RD HST_DAT0
  P m_comma
  RD HST_DAT1

  ; A block read with the 32-byte block mode on.
  call clear_sts
  P m_bl
  XFER SPD_RD, 0x00, 0x54       ; block-data | START
  TRIG
  RD HST_DAT0                   ; the count byte the EEPROM answered with

  ; A block write byte by byte: block mode off, the count in HST_DAT0, one
  ; BYTE_DONE clear per byte. The first data byte is the one HST_BLKDAT held
  ; before the start (the controller queues it there), and the last clear sends
  ; the block.
  mov dx, HST_AUX
  xor al, al
  out dx, al
  mov dx, HST_BLKD
  mov al, 0x11
  out dx, al
  call clear_sts
  P m_bw
  mov dx, HST_DAT0
  mov al, 0x02
  out dx, al
  XFER SPD_WR, 0x10, 0x55       ; block-data | START | INTREN
  RD HST_STS                    ; HOST_BUSY | BYTE_DONE
  mov dx, HST_BLKD
  mov al, 0x22
  out dx, al
  mov dx, HST_STS
  mov al, 0x80                  ; clear BYTE_DONE: queue one byte
  out dx, al
  P m_comma
  RD HST_STS
  mov dx, HST_BLKD
  mov al, 0x33
  out dx, al
  mov dx, HST_STS
  mov al, 0x80                  ; the count is reached: the block goes out
  out dx, al
  P m_comma
  RD HST_STS

  ; What that block write left at offset 0x10.
  call clear_sts
  P m_wr
  XFER SPD_RD, 0x10, 0x4c
  TRIG
  RD HST_DAT0
  P m_comma
  RD HST_DAT1

  ; An I2C block read: command through HST_DAT1, one byte per BYTE_DONE clear,
  ; LAST_BYTE ending the transfer.
  mov dx, HST_AUX
  mov al, 0x02
  out dx, al
  call clear_sts
  P m_ib
  mov dx, HST_DAT1
  mov al, 0x10
  out dx, al
  XFER SPD_RD, 0x00, 0x79       ; I2C block read | LAST_BYTE | START | INTREN
  RD HST_STS                    ; HOST_BUSY | BYTE_DONE
  P m_comma
  RD HST_BLKD                   ; the first byte the device sent
  mov dx, HST_STS
  mov al, 0x80                  ; clear BYTE_DONE with LAST_BYTE set: finish
  out dx, al
  P m_comma
  RD HST_STS

  P m_ok
  P m_crlf
  mov dx, DBGEXIT
  mov al, 5
  out dx, al
  jmp $

; Clear every status bit the guest can clear: a write of ones leaves only
; HOST_BUSY, which the controller owns.
clear_sts:
  mov dx, HST_STS
  mov al, 0xff
  out dx, al
  ret

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

; EAX as eight hex digits, most significant first.
hex32:
  push eax
  push ecx
  mov ecx, 4
.loop:
  rol eax, 8
  push eax
  call hex8
  pop eax
  dec ecx
  jnz .loop
  pop ecx
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
m_cfg:   db "smbus cfg=", 0
m_d2:    db " d2=", 0
m_st:    db " st=", 0
m_ctl:   db " ctl=", 0
m_aux:   db " aux=", 0
m_wq:    db " wq=", 0
m_rd:    db " rd=", 0
m_na:    db " na=", 0
m_im:    db " im=", 0
m_wd:    db " wd=", 0
m_bl:    db " bl=", 0
m_bw:    db " bw=", 0
m_wr:    db " wr=", 0
m_ib:    db " ib=", 0
m_comma: db ",", 0
m_ok:    db " ok", 0
m_crlf:  db 13, 10, 0
