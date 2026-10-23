# RV32 smoke probe: the paths the official rv32 p suite does not pin down.
#
#   * compressed floating-point forms that differ between RV32C and RV64C:
#     c.flw/c.fsw and c.flwsp/c.fswsp (RV64C has c.ld/c.sd there), plus
#     c.fld/c.fsd/c.fldsp/c.fsdsp (present in both, FLEN=64 with D);
#   * misa.MXL = 1 and the claimed extension bits;
#   * satp mode 1 (Sv32) round-trips and Bare clears it;
#   * the RV32 high counter halves (mcycleh/minstreth) and mstatush.
#
# This is a smoke probe (AGENTS.md 六), not an acceptance test: the rv32
# riscv-tests suite is the acceptance artifact. Exit code 0 = all checks
# passed; exit code n = check n failed (the HTIF payload is (n<<1)|1).
#
# Build: see build.sh next to this file.

.section .text.init
.globl _start
_start:
  # Check 1: misa.MXL=1 and IMAFDC+SU advertised.
  li s0, 1
  csrr t1, misa
  srli t2, t1, 30
  andi t2, t2, 3
  li t3, 1
  bne t2, t3, fail
  li t3, (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 8) | (1 << 12) | (1 << 18) | (1 << 20)
  and t2, t1, t3
  bne t2, t3, fail

  # Check 2: satp Sv32 (bit31) round-trips; writing 0 selects Bare.
  li s0, 2
  li t2, 0x80001234
  csrw satp, t2
  csrr t3, satp
  bne t2, t3, fail
  csrw satp, zero
  csrr t3, satp
  bne t3, zero, fail

  # Check 3: mstatush exists and reads 0 (no big-endian memory).
  li s0, 3
  csrr t2, mstatush
  bne t2, zero, fail

  # Check 4: high counter halves are individually writable/readable.
  li s0, 4
  li t2, 0xdeadbeef
  csrw mcycleh, t2
  csrr t3, mcycleh
  bne t2, t3, fail
  csrw mcycleh, zero
  li t2, 0x12345678
  csrw minstreth, t2
  csrr t3, minstreth
  bne t2, t3, fail
  csrw minstreth, zero

  # Enable the FP state (mstatus.FS = Dirty) for the FP checks.
  li t0, (3 << 13)
  csrs mstatus, t0

  # Check 5: c.flw / c.fsw round-trip (the RV32C word forms).
  li s0, 5
  la a0, data
  flw f2, 0(a0)
  c.flw f8, 4(a0)
  feq.s a1, f8, f2
  beqz a1, fail
  c.fsw f8, 8(a0)
  flw f3, 8(a0)
  feq.s a1, f3, f2
  beqz a1, fail

  # Check 6: c.flwsp / c.fswsp.
  li s0, 6
  addi sp, a0, 12
  c.flwsp f8, 0(sp)
  feq.s a1, f8, f2
  beqz a1, fail
  c.fswsp f8, 4(sp)
  flw f3, 16(a0)
  feq.s a1, f3, f2
  beqz a1, fail

  # Check 7: c.fld / c.fsd round-trip (RV32C+ D, 64-bit register).
  li s0, 7
  fld f2, 32(a0)
  c.fld f8, 40(a0)
  feq.d a1, f8, f2
  beqz a1, fail
  c.fsd f8, 48(a0)
  fld f3, 48(a0)
  feq.d a1, f3, f2
  beqz a1, fail

  # Check 8: c.fldsp / c.fsdsp.
  li s0, 8
  addi sp, a0, 64
  c.fldsp f8, 0(sp)
  feq.d a1, f8, f2
  beqz a1, fail
  c.fsdsp f8, 8(sp)
  fld f3, 72(a0)
  feq.d a1, f3, f2
  beqz a1, fail

  li a0, 1          # pass: HTIF payload 1 -> exit code 0
  j write_tohost

fail:
  mv a0, s0
  slli a0, a0, 1    # fail: HTIF payload (n<<1)|1 -> exit code n
  ori a0, a0, 1

write_tohost:
  la t0, tohost
  sw a0, 0(t0)
1:
  j 1b

.section .tohost, "aw", @progbits
.align 6
.globl tohost
tohost:
  .dword 0
.align 6
.globl fromhost
fromhost:
  .dword 0

.section .data
.align 3
data:
  # word block
  .word 0x3f800000        # +0  +1.0f (flw)
  .word 0x3f800000        # +4  +1.0f (c.flw)
  .word 0                 # +8  c.fsw destination
  .word 0x3f800000        # +12 +1.0f (c.flwsp source)
  .word 0                 # +16 c.fswsp destination
  .skip 12                # +20..+31
  # double block (8-byte aligned at +32)
  .dword 0x3ff0000000000000  # +32 +1.0 (fld)
  .dword 0x3ff0000000000000  # +40 +1.0 (c.fld)
  .dword 0                   # +48 c.fsd destination
  .dword 0                   # +56
  .dword 0x3ff0000000000000  # +64 +1.0 (c.fldsp)
  .dword 0                   # +72 c.fsdsp destination
