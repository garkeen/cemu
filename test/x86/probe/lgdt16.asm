; SGDT/SIDT/LGDT/LIDT operand-size semantics: a 16-bit operand keeps only the
; low 24 base bits, a 32-bit one keeps all 32 (SDM vol.2 LGDT/LIDT/SGDT/SIDT
; Operation). SeaBIOS's int 15h AH=87h depends on the 16-bit form: it passes a
; data descriptor as the pseudo-descriptor, whose byte 5 is the access byte
; (0x93), so a 32-bit load puts 0x93 into GDTR base bits 31:24 and the next
; protected-mode far jump cannot read its CS descriptor (DOS7's IO.SYS calls
; that service while booting). v86 lgdt/lidt/sgdt/sidt apply the 0xffffff mask.
;
; Output is the isa-debugcon port 0x402 (AGENTS.md section 10).
; Run (self-contained; no firmware):
;   cemu --machine x86 --isa x86 lgdt16.bin
;   qemu-system-i386 -machine pc -m 32 -drive file=lgdt16.bin,format=raw,if=floppy \
;       -boot a -display none -no-reboot \
;       -chardev file,id=dbg,path=lgdt16_qemu.txt \
;       -device isa-debugcon,iobase=0x402,chardev=dbg \
;       -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit 5 (status 11) on pass, 1 (status 3) on the first mismatch.

  bits 16
  org 0x7c00

%define DBGEXIT 0xf4
%define PDESC   0x500           ; pseudo-descriptor: limit 0x2f, base 0x5af01234
%define OUT     0x520

start:
  cli
  cld
  xor ax, ax
  mov ds, ax
  mov ss, ax
  mov sp, 0x7c00

  mov word [PDESC], 0x2f
  mov dword [PDESC + 2], 0x5af01234

  ; 16-bit LGDT/LIDT: the base byte at offset 5 (here 0x5a) is dropped.
  lgdt [PDESC]
  sgdt [OUT]
  cmp word [OUT], 0x2f
  jne fail
  cmp dword [OUT + 2], 0x00f01234
  jne fail

  lidt [PDESC]
  sidt [OUT]
  cmp word [OUT], 0x2f
  jne fail
  cmp dword [OUT + 2], 0x00f01234
  jne fail

  ; 32-bit operand size keeps all 32 base bits, in both directions.
  o32 lgdt [PDESC]
  o32 sgdt [OUT]
  cmp word [OUT], 0x2f
  jne fail
  cmp dword [OUT + 2], 0x5af01234
  jne fail

  o32 lidt [PDESC]
  o32 sidt [OUT]
  cmp word [OUT], 0x2f
  jne fail
  cmp dword [OUT + 2], 0x5af01234
  jne fail

  mov si, msg_ok
  call puts
  mov al, 5
  mov dx, DBGEXIT
  out dx, al
  jmp halt

fail:
  mov si, msg_fail
  call puts
  mov al, 1
  mov dx, DBGEXIT
  out dx, al
halt:
  hlt
  jmp halt

puts:
  lodsb
  test al, al
  jz .done
  mov dx, 0x402
  out dx, al
  jmp puts
.done:
  ret

msg_ok:   db "lgdt16 ok", 13, 10, 0
msg_fail: db "lgdt16 FAIL", 13, 10, 0

  times 510 - ($ - $$) db 0
  dw 0xaa55
