; REP-prefixed string ops with (E)CX = 0 smoke probe (not an acceptance test).
;
; SDM vol.2 "REP/REPE/REPZ/REPNE/REPNZ Prefixes" and the per-instruction
; Operation sections put the counter test in front of the loop body:
;
;     WHILE (E)CX != 0 DO ... OD
;
; so a REP-prefixed instruction entered with a zero counter performs no
; operation at all: no memory access, no pointer or counter update, no flags.
; This is not a corner case: libc-style memcpy copies the dword part with
; `shr ecx,2; rep movsd` and relies on a zero count being a no-op whenever the
; tail is 2 or 3 bytes -- exactly how isolinux's ldlinux core (do_sysappend ->
; the memcpy at guest 0x1038e0) copies a 2-byte DMI string. An emulator that
; executes the body once and decrements afterwards wraps the counter to
; 0xffffffff and runs 2^32 iterations instead of returning.
;
; Each case: pattern source at SRC, 0xaa canary at DST, counter zeroed, run the
; instruction, then require counter == 0, pointers unchanged and the canary
; intact. Prints one digit per case, '!' when that case failed, then a verdict.
;
; Dual run against qemu-system-i386 (same calibration as cd_atapi.asm): the
; transcripts and exit statuses must match.
;
; Build: nasm -f bin -o rep_zero.bin rep_zero.asm
; Run:   cemu --machine x86 --isa x86 rep_zero.bin
;        qemu-system-i386 -drive file=rep_zero.bin,format=raw,if=ide,index=0 \
;            -boot c -display none -serial stdio -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit value 5 on pass (QEMU status = (5<<1)|1 = 11), 1 on fail.

  bits 16
  org 0x7c00

%define SRC     0x8000
%define DST     0x8100
%define DBGEXIT 0xf4

%macro CHECK 0                 ; EBP = OR of the deviations of this case
  test ebp, ebp
  jz %%ok
  call set_fail
  mov al, '!'
  call putc
%%ok:
  mov al, ' '
  call putc
%endmacro

start:
  cli
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7c00
  call uart_init
  mov si, m_boot
  call puts

  ; 1: rep movsd with ECX = 0
  call fill
  mov ecx, 0
  mov esi, SRC
  mov edi, DST
  rep movsd
  xor ebp, ebp
  or ebp, ecx                ; counter untouched
  mov ebx, esi
  sub ebx, SRC
  or ebp, ebx
  mov ebx, edi
  sub ebx, DST
  or ebp, ebx
  call canary
  mov al, '1'
  call putc
  CHECK

  ; 2: rep stosd with ECX = 0
  call fill
  mov eax, 0x5a5a5a5a
  mov ecx, 0
  mov edi, DST
  rep stosd
  xor ebp, ebp
  or ebp, ecx
  mov ebx, edi
  sub ebx, DST
  or ebp, ebx
  call canary
  mov al, '2'
  call putc
  CHECK

  ; 3: rep movsb with CX = 0 (16-bit operand size)
  call fill
  mov cx, 0
  mov si, SRC
  mov di, DST
  rep movsb
  xor ebp, ebp
  movzx ebx, cx
  or ebp, ebx
  movzx ebx, si
  sub ebx, SRC
  or ebp, ebx
  movzx ebx, di
  sub ebx, DST
  or ebp, ebx
  call canary
  mov al, '3'
  call putc
  CHECK

  ; 4: repne scasb with ECX = 0 (o32: 32-bit counter)
  call fill
  mov al, 0
  mov ecx, 0
  mov edi, DST
  o32 repne scasb
  xor ebp, ebp
  or ebp, ecx
  mov ebx, edi
  sub ebx, DST
  or ebp, ebx
  mov al, '4'
  call putc
  CHECK

  call crlf
  cmp byte [fail], 0
  jne bad
  mov si, m_pass
  call puts
  mov al, 5
  jmp exit
bad:
  mov si, m_fail
  call puts
  mov al, 1
exit:
  mov dx, DBGEXIT
  out dx, al
  hlt
  jmp $                       ; no debug-exit device: stop, never wrap

; ---- helpers ---------------------------------------------------------------

; SRC = 0x11,0x22,... step 0x11; DST = 0xaa canary. Clobbers SI, DI, CX, AL.
fill:
  mov si, SRC
  mov cx, 16
  mov al, 0x11
.pat:
  mov [si], al
  inc si
  add al, 0x11
  loop .pat
  mov di, DST
  mov cx, 32
  mov al, 0xaa
.canary:
  mov [di], al
  inc di
  loop .canary
  ret

; ORs a flag into EBP when the DST canary was disturbed. Clobbers SI, CX.
canary:
  mov si, DST
  mov cx, 32
.loop:
  cmp byte [si], 0xaa
  jne .bad
  inc si
  loop .loop
  ret
.bad:
  or ebp, 1
  ret

set_fail:
  mov byte [fail], 1
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

fail:     db 0
m_boot:   db "rep-zero: ", 0
m_pass:   db "rep-zero ok", 13, 10, 0
m_fail:   db "rep-zero FAIL", 13, 10, 0

  times 510-($-$$) db 0
  dw 0xaa55
