# Smoke probe for the Sdtrig trigger module (AGENTS.md D6): mcontrol6
# matching with action=0 (breakpoint exception). Prints six digits over the
# HTIF console (dev=1, cmd=1) — bit 5 first, bit 0 last — then exits; every
# digit is '1' on pass:
#   d1 (bit 0) = execute trigger (EQ on the target pc) fires, mcause=3,
#                mtval = the target pc
#   d2 (bit 1) = load trigger (EQ on the data address) fires before the load
#   d3 (bit 2) = store trigger (EQ) fires before the store
#   d4 (bit 3) = match=GE fires at dat+2 (mtval = the accessed address) but
#                not at dat
#   d5 (bit 4) = tselect WARL-clamps to 1 (kTrigCount-1)
#   d6 (bit 5) = tdata1 WARL: match NAPOT->EQ, action 2->0, dmode unwritable
# MIE is set in the mainline because the reentrancy guard (xiangshanNEMU
# trigger_reentrancy_check) suppresses action=0 firing in M-mode while
# mstatus.MIE is clear; the trap itself clears MIE, which is what keeps the
# handler's own execution trigger-free until it disarms the trigger.
  .section .text.init
  .globl _start
_start:
  csrsi mstatus, 8            # MIE: the reentrancy guard requires it
  li s0, 0
  la t0, handler
  csrw mtvec, t0

  # d5: tselect clamps to the implemented count - 1
  li t0, 5
  csrw tselect, t0
  csrr t0, tselect
  li t1, 1
  sub t0, t0, t1
  seqz t0, t0
  slli s0, t0, 4              # bit 4

  # d6: tdata1 WARL policy on trigger 0
  csrw tselect, x0
  li t0, 0x80                 # match=1 (NAPOT) — not accepted
  csrw tdata1, t0
  csrr t0, tdata1
  srli t0, t0, 7
  andi t0, t0, 0xf            # match field reads 0 (EQ)
  seqz t0, t0
  slli t0, t0, 5
  or s0, s0, t0
  li t0, 2 << 12              # action=2 — WARLs to 0
  csrw tdata1, t0
  csrr t0, tdata1
  srli t0, t0, 12
  andi t0, t0, 0xf
  seqz t0, t0
  slli t0, t0, 6
  or s0, s0, t0
  li t0, 1 << 59              # dmode is not writable outside debug mode
  csrw tdata1, t0
  csrr t0, tdata1
  srli t0, t0, 59
  andi t0, t0, 1
  seqz t0, t0
  slli t0, t0, 7
  or s0, s0, t0

  # d1: execute trigger (m | execute) at `target`
  la s1, target
  csrw tdata2, s1
  li t0, 0x44
  csrw tdata1, t0
  sd x0, h_tval, t2           # sentinel: no trap seen yet
target:
  nop                         # the trigger fires at this fetch
  ld t0, h_tval
  bne t0, s1, bad
  li t0, 1
  or s0, s0, t0

  # d2: load trigger (m | load) at dat
  la s1, dat
  csrw tdata2, s1
  li t0, 0x41
  csrw tdata1, t0
  sd x0, h_tval, t2
  lb t3, dat                  # fires before the load
  ld t0, h_tval
  bne t0, s1, bad
  li t0, 1
  slli t0, t0, 1
  or s0, s0, t0

  # d3: store trigger (m | store) at dat
  li t0, 0x42
  csrw tdata1, t0
  sd x0, h_tval, t2
  sb t3, dat, t4              # fires before the store
  ld t0, h_tval
  bne t0, s1, bad
  li t0, 1
  slli t0, t0, 2
  or s0, s0, t0

  # d4: match=GE at dat+1 — dat+2 fires (mtval = dat+2), dat does not
  addi s1, s1, 2
  addi t0, s1, -1
  csrw tdata2, t0
  li t0, 0x141                # m | load | match=2 (GE)
  csrw tdata1, t0
  sd x0, h_tval, t2
  lb t3, dat+2                # fires, handler disarms
  ld t0, h_tval
  bne t0, s1, bad
  li t0, 1
  slli t0, t0, 3
  or s0, s0, t0
  sd x0, h_tval, t2
  lb t3, dat                  # address < tdata2: no fire
  ld t0, h_tval
  bnez t0, bad
  li t0, 1
  slli t0, t0, 4
  or s0, s0, t0

  # print s0 bits 5..0 as six digits over HTIF, then exit 0
  li t2, 0x0101000000000000
  li t3, 5
print:
  li t4, 1
  sll t4, t4, t3
  and t4, s0, t4
  snez t4, t4
  li a0, '0'
  add a0, a0, t4
  or a0, a0, t2
  sd a0, tohost, t5
  addi t3, t3, -1
  bgez t3, print
  li a0, 1
  sd a0, tohost, t5
1: j 1b

bad:
  li a0, 2                    # exit payload 2 = failure
  sd a0, tohost, t5
2: j 2b

handler:
  csrr t0, mcause
  li t1, 3                    # breakpoint exception (priv spec table 1.2)
  bne t0, t1, bad
  csrr t0, mtval
  la t1, h_tval
  sd t0, 0(t1)
  csrw tdata1, x0             # disarm: the retried instruction must pass
  mret

  .section .text.init
  .globl tohost
tohost: .dword 0
  .align 6
fromhost: .dword 0
h_tval: .dword -1
dat: .dword 0x1234
