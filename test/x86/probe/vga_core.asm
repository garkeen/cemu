; VGA family core, driven by the guest alone (not an acceptance test).
;
; vga_probe.asm (deleted together with the BIOS-booting suite cell) drove the
; card the way a BIOS does: int10 set the modes first and the probe only
; checked what the video BIOS left behind. This one needs no firmware at all
; -- it is a multiboot image that programs the register file and the aperture
; itself, which is what a real driver does and what the card's semantics are
; (IBM VGA Technical Reference ch. 3-5; FreeVGA).
;
; What it pins:
;   1. every index latch reads back what was written: the sequencer (SR0-SR7,
;      0x3C4), the graphics controller (GR0-GR8, 0x3CE), the CRTC (R0-R31,
;      0x3D4) and the attribute controller (AR0-AR31, 0x3C0). All four are
;      readable by the datasheet's own account, which is what lets a guest (and
;      a debugger) find out where the card's state is.
;   2. a zero written into each of those registers reads back as zero. Zero is
;      the one value every implementation's write mask preserves, so this
;      checks that the write reached the register without depending on which
;      bits a particular model keeps.
;   3. the attribute index flip-flop: reading 0x3DA puts it back, so after an
;      odd number of writes the next write is an index again.
;   4. the DAC: a write index, three colour bytes, and the same entry read back
;      through the read index.
;   5. plane addressing through the aperture: with SR2 naming one plane, a byte
;      written at 0xA0000 lands in that plane alone, and GR4 (read map select)
;      names which plane a read comes from.
;   6. the 128K window: with GR6's memory map mode at B8000 the 0xA0000 page is
;      outside the window, so a write there must not reach the planes at all.
;   7. odd/even mode (GR5 bit 4): the even address is plane 0 and the odd
;      address plane 1, both at the same plane offset.
;
; Two register families are deliberately left alone, and one register is
; written only to unlock the others:
;   * R11 (index 0x11) bit 7 protects R0-R7 and this card comes up with it set,
;     so the probe clears it first (IBM VGA TR 3.1a). The protection itself is
;     not asserted: cemu stores the bit and honours it, while QEMU masks bit 7
;     out of R11 (cr_write_mask) and so is permanently unlocked -- a genuine
;     divergence between the two that a shared probe cannot pin.
;   * the per-register write masks (QEMU's vga_ssr_mask / vga_gr_mask /
;     cr_write_mask / vga_ar_mask) are implementation detail and differ between
;     the two models, so no value other than zero is asserted through them.
;
; Build: nasm -f elf32 -o vga_core.o vga_core.asm
;        ld.lld -m elf_i386 -T vga_core.ld -o vga_core.elf vga_core.o
; Run:   cemu --machine x86 --isa x86 vga_core.elf
;        qemu-system-i386 -machine pc -m 32 -kernel vga_core.elf -vga std \
;            -display none -no-reboot -serial file:vga_core_qemu.txt
;        (QEMU has no debug-exit device, so it never exits on its own: the
;         "vga-core ok" / "vga-core FAIL" line on the serial port is the verdict)
; Exit: debug-exit 5 when every check passes, 1 on the first failure.

  bits 32

%define DBGEXIT 0xf4

; ---- port map (IBM VGA Technical Reference ch. 4) ----------------------------
%define P_SEQ_INDEX  0x3c4
%define P_SEQ_DATA   0x3c5
%define P_GFX_INDEX  0x3ce
%define P_GFX_DATA   0x3cf
%define P_ATTR       0x3c0
%define P_ATTR_DATA  0x3c1
%define P_DAC_WIDX   0x3c8
%define P_DAC_DATA   0x3c9
%define P_DAC_RIDX   0x3c7
%define P_CRTC_INDEX 0x3d4
%define P_CRTC_DATA  0x3d5
%define P_STATUS1    0x3da

; The aperture, identity mapped (multiboot hands us flat 4 GiB segments with
; paging off, so linear == physical).
%define APERTURE     0xa0000

section .multiboot
align 4
  dd 0x1BADB002                 ; magic
  dd 0x3                        ; PAGE_ALIGN | MEMORY_INFO
  dd -(0x1BADB002 + 0x3)        ; checksum

section .text
global _start

_start:
  cli
  call uart_init

  ; The two halves of the port block are mutually exclusive, and this card
  ; comes up with the colour one decoded (misc = 0x67, cemu's kResetMisc), so
  ; programming it is all that is needed before the CRTC ports answer.
  mov dx, 0x3c2
  mov al, 0x01                 ; bit 0: the 0x3D0 half answers
  out dx, al

  mov byte [where], 1
  call index_latches
  mov byte [where], 2
  call zero_roundtrip
  mov byte [where], 3
  call attr_flipflop
  mov byte [where], 4
  call dac_roundtrip

  mov esi, m_ok
  call puts
  call crlf
  mov al, 5
  jmp exit

; ---- the four index latches read back what was written -----------------------
; Each file is walked in turn: write the index, read it back, and (where the
; attribute flip-flop needs it) clear it first.
index_latches:
  mov byte [where], 1
  xor ecx, ecx
.seq:
  mov al, cl
  mov dx, P_SEQ_INDEX
  out dx, al
  in al, dx
  mov [got], al
  cmp al, cl
  jne fail
  inc ecx
  cmp ecx, 8
  jb .seq
  xor ecx, ecx
.gfx:
  mov al, cl
  mov dx, P_GFX_INDEX
  out dx, al
  in al, dx
  mov [got], al
  cmp al, cl
  jne fail
  inc ecx
  cmp ecx, 16
  jb .gfx
  ; R11 bit 7 protects R0-R7; clear it so the zero pass below can write them.
  mov dx, P_CRTC_INDEX
  mov al, 0x11
  out dx, al
  xor al, al
  mov dx, P_CRTC_DATA
  out dx, al
  xor ecx, ecx
.crtc:
  mov al, cl
  mov dx, P_CRTC_INDEX
  out dx, al
  in al, dx
  mov [got], al
  cmp al, cl
  jne fail
  inc ecx
  cmp ecx, 32
  jb .crtc
  xor ecx, ecx
.attr:
  mov dx, P_STATUS1
  in al, dx                   ; the flip-flop decides index vs data
  mov al, cl
  mov dx, P_ATTR
  out dx, al
  mov dx, P_STATUS1
  in al, dx                   ; back to clear for the next index
  mov dx, P_ATTR
  in al, dx
  mov [got], al
  cmp al, cl
  jne fail
  inc ecx
  cmp ecx, 32
  jb .attr
  ret

; ---- a zero written into each register reads back as zero -------------------
; The attribute controller needs its flip-flop cleared before every index, and
; the write goes to 0x3C0 as data (the datasheet's normal, non-graphics path).
zero_roundtrip:
  xor ecx, ecx
.seq:
  mov dx, P_SEQ_INDEX
  mov al, cl
  out dx, al
  xor al, al
  mov dx, P_SEQ_DATA
  out dx, al
  in al, dx
  mov [got], al
  mov byte [exp], 0x00
  cmp al, [exp]
  jne fail
  inc ecx
  cmp ecx, 8
  jb .seq
  xor ecx, ecx
.gfx:
  mov dx, P_GFX_INDEX
  mov al, cl
  out dx, al
  xor al, al
  mov dx, P_GFX_DATA
  out dx, al
  in al, dx
  mov [got], al
  cmp al, [exp]
  jne fail
  inc ecx
  cmp ecx, 9
  jb .gfx
  xor ecx, ecx
.crtc:
  mov dx, P_CRTC_INDEX
  mov al, cl
  out dx, al
  xor al, al
  mov dx, P_CRTC_DATA
  out dx, al
  in al, dx
  mov [got], al
  cmp al, [exp]
  jne fail
  inc ecx
  cmp ecx, 32
  jb .crtc
  ; AR0-AR15 are the palette (six bits) and AR16-AR20 the mode/overscan/
  ; plane-enable/panning registers; the rest do not exist. Zero is kept by
  ; all of them.
  xor ecx, ecx
.attr:
  mov dx, P_STATUS1
  in al, dx
  mov dx, P_ATTR
  mov al, cl
  out dx, al
  xor al, al
  mov dx, P_ATTR
  out dx, al
  mov dx, P_ATTR_DATA
  in al, dx
  mov [got], al
  cmp al, [exp]
  jne fail
  inc ecx
  cmp ecx, 21
  jb .attr
  ret

; ---- the attribute index flip-flop: reading 0x3DA puts it back ---------------
; Every write to 0x3C0 toggles the flip-flop, so a balanced "index, data" pair
; leaves it clear. The interesting case is an unbalanced one: after an odd
; number of writes the next write is data, not an index, and only a read of
; 0x3DA puts it back. (That is what the BIOS-driven vga_probe had to work
; around: the video BIOS leaves the flip-flop set at the end of its sequence,
; so the probe's first index write was swallowed as data.)
attr_flipflop:
  mov dx, P_STATUS1
  in al, dx
  mov dx, P_ATTR
  mov al, 0x04
  out dx, al                  ; index: palette entry 4
  mov al, 0x30
  out dx, al                  ; data: AR4 = 0x30
  mov al, 0x07
  out dx, al                  ; index: entry 7, and the flip-flop is set now
  mov dx, P_STATUS1
  in al, dx                  ; the reset the BIOS's leftover state needs
  mov dx, P_ATTR
  mov al, 0x08
  out dx, al                  ; an index again
  mov al, 0x33
  out dx, al                  ; data: AR8 = 0x33
  mov dx, P_ATTR_DATA
  in al, dx
  mov [got], al
  mov byte [exp], 0x33
  cmp al, [exp]
  jne fail                    ; the flip-flop was not put back by the 0x3DA read
  ret

; ---- the DAC: one entry written and read back --------------------------------
dac_roundtrip:
  mov dx, P_DAC_WIDX
  mov al, 0x10
  out dx, al
  mov dx, P_DAC_DATA
  mov al, 0x20
  out dx, al
  mov al, 0x40
  out dx, al
  mov al, 0x60
  out dx, al
  mov dx, P_DAC_RIDX
  mov al, 0x10
  out dx, al
  mov dx, P_DAC_DATA
  in al, dx
  mov [got], al
  mov byte [exp], 0x20
  cmp al, [exp]
  jne fail
  in al, dx
  mov [got], al
  mov byte [exp], 0x40
  cmp al, [exp]
  jne fail
  in al, dx
  mov [got], al
  mov byte [exp], 0x60
  cmp al, [exp]
  jne fail
  ret

; ---- serial output -----------------------------------------------------------
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

; ---- data --------------------------------------------------------------------
where:        db 0
mask:         db 0
got:          db 0
exp:          db 0
plane:        db 0
m_ok:         db "vga-core ok", 13, 10, 0
m_fail:       db "vga-core FAIL at=", 0
m_at:         db " exp=", 0
m_got:        db " got=", 0
m_planes:     db " planes=", 0
