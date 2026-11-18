# Instruction-address-misaligned on the jump itself when IALIGN is 32.
#
# Sail base_insts.sail `jump_to` clears only bit 0 and, when Zca is disabled,
# raises E_Fetch_Addr_Align inside the jump:
#   when target bit1 is set and Zca is off, trap(E_Fetch_Addr_Align, target)
# so mepc is the jump's own pc, mtval is the target, and the link register is
# not written. The privileged spec reports the exception on the branch/jump
# instruction, not on the target fetch.
#
# Exit: payload 1 -> status 0 when all three forms trap with that现场;
# payload 3 -> status 1 on the first mismatch.
  .section .text.init
  .globl _start
_start:
  la t0, trap
  csrw mtvec, t0

  # IALIGN = 32: clear misa.C.
  csrr t1, misa
  li t2, -5  # ~(1 << 2)
  and t1, t1, t2
  csrw misa, t1
  csrr t1, misa
  andi t2, t1, 4
  bnez t2, fail
  li s4, 0  # traps taken

  # --- 1. JALR: target bit1 set.
  la t0, tgt1
  addi s0, t0, 2       # mtval
  la s2, 1f            # mepc
  li s3, 0x55          # link register sentinel
  mv s5, s3            # expected s3 in the handler
1:
  jalr s3, 0(s0)
  li t0, 1
  bne s4, t0, fail

  # --- 2. JAL: target bit1 set.
  la t0, tgt2
  addi s0, t0, 2
  la s2, 2f
  li s3, 0x66
  mv s5, s3
2:
  jal s3, tgt2+2
  li t0, 2
  bne s4, t0, fail

  # --- 3. Taken branch: target bit1 set.
  la t0, tgt3
  addi s0, t0, 2
  la s2, 3f
  li s3, 0x77
  mv s5, s3
3:
  beq x0, x0, tgt3+2
  li t0, 3
  bne s4, t0, fail

  li a0, 1
  j write_tohost

  .align 2
trap:
  csrr t3, mcause
  bnez t3, fail
  csrr t3, mepc
  bne t3, s2, fail
  csrr t3, mtval
  bne t3, s0, fail
  bne s3, s5, fail     # link register must keep its old value
  addi s4, s4, 1
  csrr t3, mepc
  addi t3, t3, 4
  csrw mepc, t3
  mret

  # Old (masking/deferred) behaviour lands here instead of trapping.
  .align 2
tgt1:
  j fail
tgt2:
  j fail
tgt3:
  j fail

fail:
  li a0, 3

write_tohost:
  la t0, tohost
  sd a0, 0(t0)
1:
  j 1b

  .section .tohost, "aw", @progbits
  .align 6
  .globl tohost
tohost: .dword 0
  .size tohost, 8
  .align 6
  .globl fromhost
fromhost: .dword 0
  .size fromhost, 8
