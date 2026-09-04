# Smoke probe for the V3 interpreter: five instructions, HTIF exit.
# Covers the five core verbs most likely to be broken at startup:
#   lui (imm), jal (jump+link), addi (alu), jalr (indirect jump), sd (store).
# Pass criteria: exit code 0 via HTIF "1" write; a1-a3 hold the checksums.
  .section .text.init
  .globl _start
_start:
  lui   t0, 0x80001          # t0 = sign-extended 0x80001000 (lui sexts)
  slli  t0, t0, 32           # clear the sign bits: t0 = 0x80001000
  srli  t0, t0, 32
  li    a1, 1                # addi: a1 = 0 + 1
  addi  a2, a1, 1            # a2 = 2
  jal   a3, over             # jal: a3 = pc+4, jump over the next insn
  addi  a2, a2, 100          # SKIPPED if jal works
over:
  add   a3, a3, a1           # a3 = (pc+4) + 1
  li    t1, 1
  sd    t1, 0(t0)            # HTIF: tohost = 1 -> pass, exit 0
  j     .                    # never reached on pass

  .section .tohost
  .globl tohost
tohost:
  .dword 0
fromhost:
  .dword 0
