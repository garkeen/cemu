# misa's writable bits are only those whose clearing the hart actually honors
# (D35). The Extensions field is WARL and contains writable bits only where the
# implementation allows the supported ISA to be modified (privileged spec
# 3.1.1). This machine honors a cleared C (step_body.inc makes IALIGN 32 and
# rejects compressed words), but it has no path that stops entering S/U or
# executing F/D/A/M after their bits are cleared, and MXL is read-only. Those
# bits are therefore hardwired, the same outcome QEMU reaches by dropping the
# write with its default `misa_w=false` (target/riscv/csr.c write_misa).
#
# Exit: HTIF payload 1 -> status 0 when the hardwired bits survive the writes;
# payload 3 -> status 1 on the first mismatch.
  .section .text.init
  .globl _start
_start:
  csrr t0, misa

  # Clearing S and U must not stick: the machine would still enter both modes.
  li t2, ~((1 << 18) | (1 << 20))
  csrw misa, t2
  csrr t1, misa
  bne t0, t1, fail

  # Writing back only I|C must not clear MXL or the implemented extensions.
  li t2, (1 << 8) | (1 << 2)
  csrw misa, t2
  csrr t1, misa
  bne t0, t1, fail

  li a0, 1
  j write_tohost

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
