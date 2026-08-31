# Probes counter-write semantics (QEMU -M spike -icount shift=0 vs cemu).
# Prints three digits over HTIF console (dev=1, cmd=1), then exits:
#   d1 = mcycle value seen by the instruction after "csrwi mcycle, 0"
#   d2 = minstret value seen by the instruction after "csrwi minstret, 0"
#   d3 = 1 when the minstret delta across an mcycle write is exactly 1
# Spec-symmetric semantics (a counter write suppresses its own increment):
# "000". QEMU -icount shift=0 prints "110": it implements the minstret
# suppression the official test anchors (rv64mi-p-instret_overflow) but not
# the mcycle one, and its minstret freezes across an mcycle write — both
# untested corners. QEMU without -icount is not instruction-exact at all.
  .section .text.init
  .globl _start
_start:
  csrwi mcycle, 0
  csrr a1, mcycle
  csrwi minstret, 0
  csrr a2, minstret
  csrr a5, minstret
  csrwi mcycle, 0
  csrr a6, minstret
  sub a6, a6, a5
  li t1, 1
  sub a6, a6, t1
  seqz a6, a6

  li t2, 0x0101000000000000  # HTIF dev=1 cmd=1, payload in low bits
  li a0, '0'
  add a0, a0, a1
  or a0, a0, t2
  sd a0, tohost, t0
  li a0, '0'
  add a0, a0, a2
  or a0, a0, t2
  sd a0, tohost, t0
  li a0, '0'
  add a0, a0, a6
  or a0, a0, t2
  sd a0, tohost, t0

  li a0, 1                    # exit payload 1 = exit code 0
  sd a0, tohost, t0
1: j 1b

  .section .tohost, "aw", @progbits
  .align 6
  .globl tohost
tohost: .dword 0
  .size tohost, 8
  .align 6
  .globl fromhost
fromhost: .dword 0
  .size fromhost, 8
