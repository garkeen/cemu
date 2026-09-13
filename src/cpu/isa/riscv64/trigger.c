// Sdtrig trigger module: mcontrol6 address/data matching (RISC-V debug spec
// ch.5). Semantics mirrored from xiangshanNEMU
// src/isa/riscv64/system/trigger.c (mcontrol6_match, check_triggers_mcontrol6,
// trigger_handler, mcontrol6_checked_write). Two triggers exist (kTrigCount,
// both mcontrol6); action 0 raises a breakpoint exception through the raise
// channel and action 1 would enter debug mode, which this machine has no
// implementation of, so firing it stops the run loudly.
//
// The check points mirror the reference: execute triggers at the fetched pc
// before the instruction runs (step.c), load/store triggers on the effective
// address before the access (exec.c funnels). mtval on a fired breakpoint
// carries the matched address (the pc for execute triggers).
#include "riscv.h"
#include "util/log.h"

// One mcontrol6's match decision (debug spec 5.3.12; xiangshanNEMU
// mcontrol6_match). select is WARL-0, so the compared value is always the
// operation's address.
static int Mc6Match(RiscvState* s, uint64_t t1, uint64_t t2, uint64_t t3, int op,
                    uint64_t addr) {
  if ((!(op & kTrigOpExecute) || !(t1 & kMc6Execute)) &&
      (!(op & kTrigOpLoad) || !(t1 & kMc6Load)) &&
      (!(op & kTrigOpStore) || !(t1 & kMc6Store)))
    return 0;
  // Privilege gates: the trigger fires only in the modes it enables. The
  // vu/vs bits gate VS/VU, which this machine never runs (no H).
  uint64_t want = s->priv == kPrivMachine ? kMc6M
                  : s->priv == kPrivSupervisor ? kMc6S
                                               : kMc6U;
  if (!(t1 & want)) return 0;
  // tdata3 qualification (debug spec 5.3.10; xiangshanNEMU tdata3_smatch):
  // sselect 0 matches always, 1 compares scontext against svalue byte-wise
  // (a set sbytemask bit marks that byte don't-care), 2 compares satp.ASID;
  // every other encoding never matches.
  uint64_t ssel = t3 & kTd3SselectMask;
  if (ssel == 1) {
    uint32_t sval = (uint32_t)(t3 >> kTd3SvalueShift);
    uint32_t smask = (uint32_t)((t3 >> kTd3SbytemaskShift) & 0xf);
    uint32_t ctx = (uint32_t)s->scontext;
    for (int b = 0; b < 4; b++)
      if (!((smask >> b) & 1) && ((ctx >> (8 * b)) & 0xff) != ((sval >> (8 * b)) & 0xff))
        return 0;
  } else if (ssel == 2) {
    if (((s->satp >> 44) & 0xffff) != ((t3 >> kTd3SvalueShift) & 0xffff)) return 0;
  } else if (ssel != 0) {
    return 0;
  }
  switch ((t1 >> 7) & 0xf) {
    case kMc6MatchEq:
      return addr == t2;
    case kMc6MatchGe:
      return addr >= t2;
    default:  // kMc6MatchLt — the only encoding the write policy accepts here
      return addr < t2;
  }
}

// Reentrancy guard (xiangshanNEMU trigger_reentrancy_check): a breakpoint
// that would trap into a level with interrupts disabled would refire on the
// handler's own fetch forever — don't fire it.
static int TrigReentryOk(RiscvState* s) {
  int bp_deleg = (int)((s->medeleg >> kExBreakpoint) & 1);
  if (s->priv == kPrivMachine && !(s->mstatus & kMstatusMie)) return 0;
  if (s->priv == kPrivSupervisor && bp_deleg && !(s->mstatus & kMstatusSie)) return 0;
  return 1;
}

// action 0 = breakpoint exception (mtval carries the matched address);
// action 1 = enter debug mode, which this machine does not implement (no
// dcsr/dpc state) — stop loudly.
_Noreturn static void Fire(frame* fr, uint64_t t1, uint64_t addr) {
  if (((t1 >> 12) & 0xf) == 1) Fatal("riscv: trigger action 1 (debug mode) fired");
  raise_(fr, kExBreakpoint, addr);
}

void RiscvTriggerCheck(frame* fr, RiscvState* s, int op, uint64_t addr) {
  if (!TrigReentryOk(s)) return;
  // All hits are evaluated first: a chained trigger fires only when the
  // previous trigger in the chain matched the same operation (debug spec
  // 5.3.12 chain; xiangshanNEMU check_triggers_mcontrol6).
  int hit0 = Mc6Match(s, s->tdata1[0], s->tdata2[0], s->tdata3[0], op, addr);
  int hit1 = Mc6Match(s, s->tdata1[1], s->tdata2[1], s->tdata3[1], op, addr);
  int chain0 = (int)((s->tdata1[0] >> 11) & 1);
  if (hit0 && !chain0) Fire(fr, s->tdata1[0], addr);
  if ((hit0 || !chain0) && hit1 && !((s->tdata1[1] >> 11) & 1)) Fire(fr, s->tdata1[1], addr);
}

// tdata1 write (debug spec 5.3.12; xiangshanNEMU mcontrol6_checked_write):
// the type is fixed to mcontrol6, dmode can only be cleared from outside
// debug mode, select is WARL-0 (no data-value matching), only match EQ/GE/LT
// and action 0/1 are accepted (others WARL to EQ/0), the chain bit is kept
// only where a successor trigger exists, and the hit/size/uncertain fields
// read 0 after a write. tselect selects the trigger (Sdtrig).
void RiscvTriggerWriteTdata1(RiscvState* s, uint64_t val) {
  int idx = (int)s->tselect;
  uint64_t cur = s->tdata1[idx];
  uint64_t v = kTrigTypeMcontrol6;
  v |= cur & val & kMc6Dmode;
  uint64_t match = (val >> 7) & 0xf;
  if (match == kMc6MatchEq || match == kMc6MatchGe || match == kMc6MatchLt) v |= match << 7;
  uint64_t action = (val >> 12) & 0xf;
  if (action <= 1) v |= action << 12;
  // A chained trigger with no successor can never fire: the last trigger's
  // chain bit WARLs to 0 (xiangshanNEMU mcontrol6_check_chain_legal).
  if (idx < kTrigCount - 1 || !(val & kMc6Chain)) v |= val & kMc6Chain;
  v |= val & (kMc6Load | kMc6Store | kMc6Execute | kMc6U | kMc6S | kMc6M);
  s->tdata1[idx] = v;
}

// tdata3 write (debug spec 5.3.10): mhselect/mhvalue are WARL-0 without the
// H extension; the Sscontext fields store as written (the match above limits
// which sselect encodings can ever fire). tselect selects the trigger.
void RiscvTriggerWriteTdata3(RiscvState* s, uint64_t val) {
  s->tdata3[s->tselect] = val & ~kTd3MhMask;
}
