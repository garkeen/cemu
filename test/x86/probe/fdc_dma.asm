; Floppy controller + 8237 DMA, driven by the guest alone (not an acceptance
; test).
;
; What the old fddsmoke.asm could not do: SeaBIOS did all the 8272A and 8237
; programming, so it only checked the firmware-to-medium path. This is a
; multiboot image that issues the controller's commands itself (82077AA
; datasheet ch. 9) and programs DMA channel 2 itself (8237A datasheet), so it
; pins the two devices rather than the firmware.
;
; The whole probe is one line of story, and each step is only here because the
; next one needs it:
;   reset the controller and retire the four interrupts it raises -> SPECIFY
;   (no result, no interrupt) -> SEEK to cylinder 0 and retire that interrupt
;   -> program channel 2 -> READ DATA one sector -> check the 512 bytes ->
;   WRITE DATA a different pattern -> READ DATA again -> check it came back.
;
; The medium is a blank 1.44 MiB raw image the suite generates itself (one
; "dd if=/dev/zero bs=512 count=2880"; no firmware, no operating system
; image), handed over with -fda -- the only way a floppy controller can be
; given a disk at all. Sector 1 is therefore all zeros, and the read is proved
; by pre-filling the buffer with 0xFF first.
;
; Two facts this file had to get right, both read off the code rather than
; guessed:
;   * the command/status register is 0x08, NOT 0x82: 0x81-0x8F is the page
;     latch block, so a read at 0x82 answers with channel 3's page register
;     (i8237.c DecodePort / PageChannel).
;   * the mode register's direction field (bits 3:2) decides which way the
;     controller walks guest memory: 01 (0x46 in single mode on channel 2) has
;     it write memory, which is what reading a sector needs; 10 (0x4a) is the
;     disk draining memory (i8237.c kI8237DirWrite / kI8237DirRead). SeaBIOS
;     src/hw/dma.c dma_floppy programs that same pair, 0x46 for a read and
;     0x4a for a write.
;
; Build: nasm -f elf32 -o fdc_dma.o fdc_dma.asm
;        ld.lld -m elf_i386 -T fdc_dma.ld -o fdc_dma.elf fdc_dma.o
; Run:   cemu --machine x86 --isa x86 -fda <image> fdc_dma.elf
; Exit: debug-exit 5 when the sector moved both ways, 1 on the first failure
;       (the report carries which step and what was expected).

  bits 32

%define DBGEXIT 0xf4

%define FDC_SRA     0x3f0        ; status register A, bit 7 = interrupt pending
%define FDC_DOR     0x3f2        ; drives, reset, DMA/IRQ enable, motors
%define FDC_MSR     0x3f4        ; main status: RQM / DIO / command busy
%define FDC_FIFO    0x3f5        ; command / parameter / result register

%define DOR_ON      0x1c         ; out of reset, DMA and IRQ on, motor 0

%define CMD_SENSE   0x08         ; SENSE INTERRUPT STATUS
%define CMD_SPECIFY 0x03
%define CMD_SEEK    0x0f
%define CMD_READ    0x46         ; MFM + READ DATA
%define CMD_WRITE   0x45         ; MFM + WRITE DATA

%define DMA_ADDR    0x04         ; channel 2 address
%define DMA_CNT     0x05         ; channel 2 word count
%define DMA_STATUS  0x08         ; command/status: bits 0-3 = channel TC
%define DMA_MODE    0x0b
%define DMA_CLEAR   0x0c         ; clear the shared byte flip-flop
%define DMA_MASTER  0x0f         ; the four mask bits
%define DMA_PAGE    0x81         ; page latch for channel 2

%define DMA_TO_MEM   0x46        ; single mode, channel 2, device -> memory
%define DMA_FROM_MEM 0x4a        ; single mode, channel 2, memory -> device

%define TC_CH2      0x04         ; the status register's channel-2 TC bit

%define SECTOR      512
%define DMA_COUNT   (SECTOR - 1) ; the count register holds N-1
%define BUFFER      0x20000      ; where the transfer lands (identity mapped)
%define PAT_MEDIUM  0x00         ; a blank image: sector 1 is all zeros
%define PAT_WRITE   0x55         ; what the guest puts on the medium
%define FILL        0xff         ; what the buffer holds before a read

; One byte into the controller's data register. The argument is always a
; number or one of the command constants -- never a symbol, because nasm
; turns `mov al, <symbol>` into a displacement-sized immediate and the link
; then fails with R_386_8 out of range.
%macro W 1
  call writable
  mov dx, FDC_FIFO
  mov al, %1
  out dx, al
%endmacro

; One result byte out of the data register, with its own wait: the register
; faces the guest only while a byte is waiting (RQM set, DIO set).
%macro RB 1
  call readable
  mov dx, FDC_FIFO
  in al, dx
  mov [%1], al
%endmacro

; The masked bits of a result byte must all be zero.
%macro CHECK_MASK 3
  mov al, [%1]
  mov [got], al
  and al, %2
  jz %%ok
  mov byte [exp], %2
  mov byte [where], %3
  jmp fail
%%ok:
%endmacro

; A result byte must equal a literal.
%macro CHECK_BYTE 3
  mov al, [%1]
  mov [got], al
  mov byte [exp], %2
  cmp al, %2
  je %%ok
  mov byte [where], %3
  jmp fail
%%ok:
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

  ; --- reset, and the four interrupts it raises ------------------------------
  mov byte [where], 1
  mov dx, FDC_DOR
  xor al, al
  out dx, al                    ; hold the controller in reset
  call settle
  mov al, DOR_ON
  out dx, al                    ; and let it out: this edge raises IRQ
  call settle
  mov dx, FDC_SRA
  in al, dx
  test al, 0x80
  jz fail                       ; no interrupt from the reset
  ; One SENSE INTERRUPT STATUS per logical drive (82077AA 7.4). Their bodies
  ; are not checked: the point here is to retire the four pending interrupts so
  ; the SEEK's interrupt is the only one the waits below can see.
  mov ecx, 4
.drain:
  W CMD_SENSE
  RB res0                       ; ST0
  RB res1                       ; PCN
  dec ecx
  jnz .drain

  ; --- to cylinder 0, then program the channel and read the sector -----------
  mov byte [where], 2
  W CMD_SPECIFY
  W 0xdf                       ; step rate, head load time
  W 0x02                       ; head unload time, no DMA on the controller
  W CMD_SEEK
  W 0x00                       ; head 0, drive 0
  W 0x00                       ; cylinder 0
  call wait_irq
  W CMD_SENSE
  RB res0                       ; ST0
  RB res1                       ; PCN: the cylinder the seek ended on
  CHECK_BYTE res1, 0x00, 2      ; the seek must have ended on cylinder 0

  call fill
  call dma_to_memory
  W CMD_READ
  W 0x00                       ; HD/US: head 0, drive 0
  W 0x00                       ; cylinder 0
  W 0x00                       ; head 0
  W 0x01                       ; sector 1
  W 0x02                       ; 512 bytes per sector
  W 0x01                       ; EOT: the last sector to read
  W 0x1b                       ; gap length
  W 0xff                       ; data length: unused at a 512-byte size code
  call wait_irq
  call dma_tc
  call read_seven
  call check_termination
  call expect_buffer             ; the medium's zeros must have arrived

  ; --- the same sector the other way, and read it back -----------------------
  mov byte [where], 3
  mov al, PAT_WRITE
  call fill_with
  call dma_from_memory
  W CMD_SPECIFY
  W 0xdf
  W 0x02
  W CMD_SEEK
  W 0x00
  W 0x00
  call wait_irq
  W CMD_SENSE
  RB res0
  RB res1
  W CMD_WRITE
  W 0x00
  W 0x00
  W 0x00
  W 0x01
  W 0x02
  W 0x01
  W 0x1b
  W 0xff
  call wait_irq
  call dma_tc
  call read_seven
  call check_termination

  mov byte [where], 4
  mov al, FILL
  call fill_with
  call dma_to_memory
  W CMD_SPECIFY
  W 0xdf
  W 0x02
  W CMD_SEEK
  W 0x00
  W 0x00
  call wait_irq
  W CMD_SENSE
  RB res0
  RB res1
  W CMD_READ
  W 0x00
  W 0x00
  W 0x00
  W 0x01
  W 0x02
  W 0x01
  W 0x1b
  W 0xff
  call wait_irq
  call dma_tc
  call read_seven
  call check_termination
  call expect_written           ; the 0x55 must have come back off the medium

  ; The address bytes (C/H/R/N) that check_termination has just verified, kept
  ; on the record as a one-line transcript: the golden and the cemu log are
  ; diffed by eye, and this line is where a divergence would show first.
  mov esi, m_pos
  call puts
  mov al, [res3]
  call puthex8
  mov al, [res4]
  call puthex8
  mov al, [res5]
  call puthex8
  mov al, [res6]
  call puthex8
  call crlf

  mov esi, m_ok
  call puts
  call crlf
  mov al, 5
  jmp exit

; === the 8237 channel ========================================================
; Address, count and page latch, then the mode register (which is what tells
; the controller which way to walk memory), and finally the mask bits.
dma_to_memory:
  call dma_address_and_count
  mov dx, DMA_MODE
  mov al, DMA_TO_MEM
  out dx, al
  jmp dma_unmask

dma_from_memory:
  call dma_address_and_count
  mov dx, DMA_MODE
  mov al, DMA_FROM_MEM
  out dx, al

dma_unmask:
  mov dx, DMA_MASTER
  xor al, al
  out dx, al                    ; the mask bits as written: 0 clears them all
  ret

dma_address_and_count:
  mov dx, DMA_CLEAR
  xor al, al
  out dx, al                    ; low byte first
  mov dx, DMA_ADDR
  mov al, BUFFER & 0xff
  out dx, al
  mov al, (BUFFER >> 8) & 0xff
  out dx, al
  mov dx, DMA_CLEAR
  xor al, al
  out dx, al
  mov dx, DMA_CNT
  mov al, DMA_COUNT & 0xff
  out dx, al
  mov al, DMA_COUNT >> 8
  out dx, al
  mov dx, DMA_PAGE
  mov al, (BUFFER >> 16) & 0xff
  out dx, al
  ret

; Terminal count: the status register's channel-2 bit latches when the count
; runs out (8237A datasheet, Status Register). Each transfer asks for exactly
; the programmed 512 bytes, so the bit must be up after every command.
dma_tc:
  mov dx, DMA_STATUS
  in al, dx
  mov [got], al
  and al, TC_CH2
  jz .no_tc
  ret
.no_tc:
  mov byte [exp], TC_CH2
  mov byte [where], 10
  jmp fail

; === the buffer =============================================================
; fill_with uses AL, so the caller sets it and this only stores it.
fill_with:
  mov ecx, SECTOR
  mov edi, BUFFER
  rep stosb
  ret

fill:
  mov al, FILL
  jmp fill_with

; The two expectations, so a failure says which direction broke.
expect_buffer:
  mov byte [want], PAT_MEDIUM
  jmp compare

expect_written:
  mov byte [want], PAT_WRITE

compare:
  mov ecx, SECTOR
  mov edi, BUFFER
.next:
  mov al, [edi]
  cmp al, [want]
  jne .bad
  inc edi
  dec ecx
  jnz .next
  ret
.bad:
  mov [got], al
  mov al, [want]
  mov [exp], al
  mov byte [where], 9           ; 9 = the data itself is wrong
  jmp fail

; === result bytes ==========================================================
; READ/WRITE DATA leaves seven result bytes -- ST0, ST1, ST2, C, H, R, N
; (8272A datasheet, "Result Phase") -- and the controller only returns to the
; command phase once the guest has taken the last one (fdc.c ReadFifo), so all
; seven are read even where only some are checked.
read_seven:
  RB res0                       ; ST0
  RB res1                       ; ST1
  RB res2                       ; ST2
  RB res3                       ; C
  RB res4                       ; H
  RB res5                       ; R
  RB res6                       ; N
  ret

; What a normal termination reports: the three status registers with none of
; their error bits set -- ST0's IC = 00, EC and NR clear; ST1's EN, DE, OR,
; ND, NW, MA clear; ST2's MD, BC, SN, SH, WC clear (8272A datasheet, Status
; Registers 0-2) -- and the address of where the head ended up. ST0's SE bit
; is deliberately not checked: every command here runs to EOT, which steps the
; head to the next cylinder, and the model reports that step as seek end
; (fdc.c AdvanceHead sets kSr0Seek). That stepped-to address is checked: all
; three commands run from C0/H0/R1, so the result must read C1/H0/R1 with the
; 512-byte size code -- calibrated by the dual run, whose pos= line is the
; same on cemu and QEMU.
check_termination:
  CHECK_MASK res0, 0xd8, 5      ; IC (7:6), EC (4), NR (3)
  CHECK_MASK res1, 0xbd, 6      ; EN, DE, OR, ND, NW, MA
  CHECK_MASK res2, 0x7c, 7      ; MD, BC, SN, SH, WC
  CHECK_BYTE res3, 0x01, 8      ; the EOT step moved the head to cylinder 1
  CHECK_BYTE res4, 0x00, 8      ; head 0
  CHECK_BYTE res5, 0x01, 8      ; sector 1
  CHECK_BYTE res6, 0x02, 8      ; size code 2 = 512 bytes
  ret

; === waits ==================================================================
; The guest may write the data register: RQM set and DIO clear.
writable:
  push eax
  push ecx
  mov ecx, 200000
.wait:
  mov dx, FDC_MSR
  in al, dx
  test al, 0x80
  jz .again
  test al, 0x40
  jz .ok
.again:
  dec ecx
  jnz .wait
  pop ecx
  pop eax
  jmp fail
.ok:
  pop ecx
  pop eax
  ret

; The result phase: a byte is waiting in the data register.
readable:
  push eax
  push ecx
  mov ecx, 200000
.wait:
  mov dx, FDC_MSR
  in al, dx
  test al, 0x80
  jz .again
  test al, 0x40
  jnz .ok
.again:
  dec ecx
  jnz .wait
  pop ecx
  pop eax
  jmp fail
.ok:
  pop ecx
  pop eax
  ret

; The controller's own "interrupt pending" bit.
wait_irq:
  push ecx
  mov ecx, 4000000
.wait:
  mov dx, FDC_SRA
  in al, dx
  test al, 0x80
  jnz .done
  dec ecx
  jnz .wait
  pop ecx
  jmp fail
.done:
  pop ecx
  ret

settle:
  push ecx
  mov ecx, 20000
.loop:
  dec ecx
  jnz .loop
  pop ecx
  ret

; === serial output ==========================================================
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

puthex8:
  push eax
  shr al, 4
  call .nib
  pop eax
  and al, 0x0f
.nib:
  cmp al, 10
  jb .digit
  add al, 'a' - 10
  jmp .emit
.digit:
  add al, '0'
.emit:
  push eax
  call putc
  pop eax
  ret

putc:
  push eax
  mov dx, 0x3fd
.wait:
  in al, dx
  test al, 0x20
  jz .wait
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

fail:
  mov esi, m_fail
  call puts
  mov al, [where]
  call puthex8
  mov esi, m_at
  call puts
  mov al, [exp]
  call puthex8
  mov esi, m_got
  call puts
  mov al, [got]
  call puthex8
  call crlf
  mov al, 1
  jmp exit

exit:
  mov dx, DBGEXIT
  out dx, al
  hlt
  jmp $

; === data ===================================================================
; where: 1 = reset, 2 = the seek (and the read after it), 3 = write,
;        4 = read-back, 5/6/7 = the ST0/ST1/ST2 mask checks, 8 = the result
;        address, 9 = the sector data, 10 = the DMA channel never reached
;        terminal count.
where:       db 0
got:         db 0
exp:         db 0
want:        db 0
res0:        db 0
res1:        db 0
res2:        db 0
res3:        db 0
res4:        db 0
res5:        db 0
res6:        db 0
m_pos:       db "fdc-dma pos=", 0
m_ok:        db "fdc-dma ok", 13, 10, 0
m_fail:      db "fdc-dma FAIL at=", 0
m_at:        db " exp=", 0
m_got:       db " got=", 0

