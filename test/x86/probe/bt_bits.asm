; BT/BTS/BTR/BTC bit-string addressing smoke probe (not an acceptance test).
;
; SDM vol.2 BT: with a *memory* bit base the offset indexes a bit string --
; but the two encodings divide the work differently, and that is the whole
; point of this probe:
;
;   * Register bit offset (0F A3/AB/B3/BB). The processor itself advances:
;         Effective Address + (4 * (BitOffset DIV 32))     ; 32-bit operand
;         Effective Address + (2 * (BitOffset DIV 16))     ; 16-bit operand
;     and tests bit (BitOffset MOD OperandSize) there. It may read 4 bytes
;     even when one byte would do. A register bit base never moves: it takes
;     the offset modulo the operand size.
;
;   * Immediate bit offset (0F BA /4-/7). The high-order bits are the
;     assembler's business -- "some assemblers support immediate bit offsets
;     larger than 31 by using the immediate bit offset field in combination
;     with the displacement field ... the processor will ignore the high order
;     bits if they are not zero" (SDM vol.2 BT). So the processor masks the
;     immediate to 3 or 5 bits and does NOT advance the address; nasm does not
;     fold the high bits into the displacement, so `bt dword [m], 40` tests
;     bit 8 of m, not of m+4.
;
; Both halves are asserted, so this probe fails on either kind of wrong
; answer: an emulator that only masks the offset in the *register* form reads
; the wrong dword for every offset >= 32, which is exactly how the linux.iso
; kernel's init_IRQ() came to install zero IRQ gates -- the system_vectors
; bitmap test read the same dword for all 224 vectors (stage 4 片 12, whose
; fix this guards). An emulator that advances in the *immediate* form is the
; mirror-image error (tiny386's BTEvIb does this; the manual arbitrates, per
; AGENTS.md §五).
;
; Buffers: B32 = two dwords [0] = 0, [1] = 0xffffffff; B16 = two words
; [0] = 0, [1] = 0x0010. Every case picks a bit whose value differs between
; the first and the second operand, so reading the wrong one changes the CF
; the case demands. Prints one digit per case, '!' when that case failed,
; then a verdict.
;
; Dual run against qemu-system-i386 (same calibration as rep_zero.asm): the
; transcripts and exit statuses must match.
;
; Build: nasm -f bin -o bt_bits.bin bt_bits.asm
; Run:   cemu --machine x86 --isa x86 bt_bits.bin
;        qemu-system-i386 -drive file=bt_bits.bin,format=raw,if=ide,index=0 \
;            -boot c -display none -serial stdio -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Exit: debug-exit value 5 on pass (QEMU status = (5<<1)|1 = 11), 1 on fail.

  bits 16
  org 0x7c00

%define B32     0x8000
%define B16     0x8100
%define DBGEXIT 0xf4

; The judge is the CF the SDM requires; a mismatch marks the case and prints
; '!'. Taking the branch directly keeps the passing path at two bytes, which
; is what makes nine cases fit in a 512-byte sector.
%macro WANT 1
  %if %1
    jc %%ok
  %else
    jnc %%ok
  %endif
  call mark_bad
%%ok:
%endmacro

%macro WANTMEM32 2              ; %1 = address, %2 = expected dword
  cmp dword [%1], %2
  je %%ok
  call mark_bad
%%ok:
%endmacro

%macro WANTMEM16 2              ; %1 = address, %2 = expected word
  cmp word [%1], %2
  je %%ok
  call mark_bad
%%ok:
%endmacro

%macro SEP 0                    ; close a case: separator between digits
  mov al, ' '
  call putc
%endmacro

%macro INITB32 0
  mov dword [B32], 0
  mov dword [B32 + 4], 0xffffffff
%endmacro

%macro INITB16 0
  mov word [B16], 0
  mov word [B16 + 2], 0x0010
%endmacro

start:
  cli
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7c00
  call uart_init

  INITB32
  ; 1: bt dword [B32], eax with eax = 40 -- the register form advances to the
  ;    second dword: bit 8 of [B32+4] = 1 (bit 8 of [B32] is 0)
  mov eax, 40
  bt dword [B32], eax
  WANT 1
  mov al, '1'
  call putc
  SEP

  ; 2: eax = 32 -- exactly on the boundary: bit 0 of the second dword
  mov eax, 32
  bt dword [B32], eax
  WANT 1
  mov al, '2'
  call putc
  SEP

  ; 3: eax = 31 -- still inside the first operand (31 DIV 32 = 0)
  mov eax, 31
  bt dword [B32], eax
  WANT 0
  mov al, '3'
  call putc
  SEP

  INITB32
  ; 4: bts dword [B32], eax with eax = 33 -- sets bit 1 of the second dword
  mov eax, 33
  bts dword [B32], eax
  WANT 1
  WANTMEM32 B32, 0            ; the first dword must not be touched
  mov al, '4'
  call putc
  SEP

  ; 5: btr dword [B32], eax with eax = 33 -- clears that bit again
  mov eax, 33
  btr dword [B32], eax
  WANT 1
  WANTMEM32 B32, 0
  WANTMEM32 B32 + 4, 0xfffffffd
  mov al, '5'
  call putc
  SEP

  INITB32
  ; 6: btc dword [B32], eax with eax = 63 -- toggles bit 31 of the second dword
  mov eax, 63
  btc dword [B32], eax
  WANT 1
  WANTMEM32 B32, 0
  WANTMEM32 B32 + 4, 0x7fffffff
  mov al, '6'
  call putc
  SEP

  ; 7: bt dword [B32], 40 -- the IMMEDIATE form does not advance: the high
  ;    bits of the offset are ignored, so this is bit 8 of the first dword
  bt dword [B32], 40
  WANT 0
  mov al, '7'
  call putc
  SEP

  INITB16
  ; 8: bt word [B16], ax with ax = 20 -- 16-bit operand: the register form
  ;    advances 2 bytes, bit 4 of the second word (0x0010) is 1
  mov ax, 20
  bt word [B16], ax
  WANT 1
  mov al, '8'
  call putc
  SEP

  ; 9: bt word [B16], 20 -- immediate form again: bit 4 of the first word = 0
  bt word [B16], 20
  WANT 0
  mov al, '9'
  call putc
  SEP

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

mark_bad:
  mov byte [fail], 1
  mov al, '!'
  jmp putc

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
m_pass:   db "bt-bits ok", 13, 10, 0
m_fail:   db "bt-bits FAIL", 13, 10, 0

  times 510-($-$$) db 0
  dw 0xaa55
