; Reset-facility probe for the x86_min machine (AGENTS.md D18). The three ways
; a PC guest reboots the machine are pulled in turn, one per life:
;
;   life 1  port 0xCF9, the PIIX3 reset control register (SeaBIOS pci_reboot:
;           write 2, then 6 — the second write is the one that acts)
;   life 2  port 0x92 bit 0, INIT_NOW (SeaBIOS opens A20 here; bit 0 reboots)
;   life 3  port 0x64 command 0xFE, the keyboard controller's reset line
;           (SeaBIOS i8042_reboot)
;   life 4  prints "reset ok" and exits through the debug-exit port
;
; A reset restarts the machine, not the memory: the boot sector keeps its life
; counter in CMOS, which is battery-backed on a real board and survives a
; reset in any machine that models the chip. Low RAM does not work for this:
; the firmware runs again between the resets and uses it. Only a machine that
; actually restarted can reach life 4; one whose reset requests are ignored
; hangs in life 1 and the run ends on the suite's timeout.
;
; TRIGGER selects what the image does, which is how each trigger is measured
; against another emulator on its own (they do not all implement all three):
;   (default) the three-life sequence above
;   1 / 2 / 3 one trigger only, then "no reset" + exit — printing it means the
;             trigger did not reset the machine.
;   nasm -f bin -DTRIGGER=2 -o reset_port92.bin reset.asm
;
; Loaded at 0x7C00 with CS:IP = 0000:7C00 and DL=0x80, exactly like the smoke
; probe; exits with debug-exit payload 5 (emulator status = (5<<1)|1 = 11).
;
; Build: nasm -f bin -o reset.bin reset.asm
; Run:   cemu --machine x86 --isa x86 reset.bin
;        qemu-system-i386 -drive file=reset.bin,format=raw,if=ide \
;            -display none -serial file:reset_qemu.txt \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
  bits 16
  org 0x7c00

%ifndef TRIGGER
%define TRIGGER 0
%endif

kCmosMagic   equ 0x40   ; 'R' once this image has run at least once
kCmosCounter equ 0x41   ; how many lives it has had
kCom1     equ 0x3f8
kLsr      equ 0x3fd
kDebugExit equ 0xf4

start:
  cli
  xor ax, ax
  mov ds, ax
  mov ss, ax
  mov sp, 0x7c00

  ; The magic tells a fresh machine from one that has already been reset; the
  ; counter then picks this life's trigger.
  mov bl, kCmosMagic
  call cmos_read
  cmp al, 0x52           ; 'R'
  je .count
  mov bl, kCmosMagic
  mov al, 0x52
  call cmos_write
  mov bl, kCmosCounter
  xor al, al
  call cmos_write
.count:
  mov bl, kCmosCounter
  call cmos_read
  inc al
  call cmos_write        ; AL = this life's number, BL = the counter register
%if TRIGGER == 0
  cmp al, 1
  je .cf9
  cmp al, 2
  je .port92
  cmp al, 3
  je .kbd
  jmp .done
%elif TRIGGER == 1
  jmp .cf9
%elif TRIGGER == 2
  jmp .port92
%else
  jmp .kbd
%endif

.cf9:
  mov al, 2              ; reset type: hard reset
  mov dx, 0xcf9
  out dx, al
  mov al, 6              ; bit 2: do the reset
  out dx, al
%if TRIGGER != 0
  jmp .noreset
%endif
  jmp .hang
.port92:
  mov al, 1              ; bit 0: INIT_NOW
  out 0x92, al
%if TRIGGER != 0
  jmp .noreset
%endif
  jmp .hang
.kbd:
  mov al, 0xfe           ; keyboard controller reset command
  out 0x64, al
%if TRIGGER != 0
  jmp .noreset
%endif
  jmp .hang

.hang:
  hlt
  jmp .hang

%if TRIGGER != 0
.noreset:
  mov si, msg_noreset
  jmp .print
%endif

.done:
  mov si, msg
.print:
  ; COM1 init: divisor 1 (115200 baud), 8N1, interrupts off
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

.next:
  lodsb
  test al, al
  jz .exit
  call putc
  jmp .next

.exit:
  mov al, 5              ; exit payload: emulator status = (5<<1)|1 = 11
  mov dx, kDebugExit
  out dx, al
  jmp .hang

putc:                    ; AL = character, waits for THRE
  push ax
  mov dx, kLsr
.wait:
  in al, dx
  test al, 0x20
  jz .wait
  pop ax
  mov dx, kCom1
  out dx, al
  ret

cmos_read:               ; BL = register -> AL
  mov al, bl
  out 0x70, al
  in al, 0x71
  ret

cmos_write:              ; AL = value, BL = register
  push ax
  mov al, bl
  out 0x70, al
  pop ax
  out 0x71, al
  ret

msg: db "reset ok", 13, 10, 0
msg_noreset: db "no reset", 13, 10, 0

  times 510-($-$$) db 0
  dw 0xaa55
