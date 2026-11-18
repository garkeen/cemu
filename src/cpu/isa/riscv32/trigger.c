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
#include "riscv32.h"
#include "util/log.h"

// One mcontrol6's match decision (debug spec 5.3.12; xiangshanNEMU
// mcontrol6_match). select is WARL-0, so the compared value is always the
// operation's address.
static int Mc6Match(Riscv32State* s, uint64_t t1, uint64_t t2, uint64_t t3, int op,
                    uint64_t addr, int size) {
  if (((t1 >> 28) & 0xf) != 6) return 0;  // only mcontrol6 is implemented
  uint64_t sz = (t1 >> 16) & 7;
  if (sz != 0) {
    int want = sz == 1 ? 1 : sz == 2 ? 2 : sz == 3 ? 4 : sz == 4 ? 8 : 0;
    if (want == 0 || size != want) return 0;
  }
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
    // RV32 tdata3 layout: svalue 17:2, sbytemask 19:18, two context bytes.
    uint32_t sval = (uint32_t)(t3 >> kTd3SvalueShift) & 0xffffu;
    uint32_t smask = (uint32_t)((t3 >> kTd3SbytemaskShift) & 0x3u);
    uint32_t ctx = (uint32_t)s->scontext & 0xffffu;
    for (int b = 0; b < 2; b++)
      if (!((smask >> b) & 1) && ((ctx >> (8 * b)) & 0xff) != ((sval >> (8 * b)) & 0xff))
        return 0;
  } else if (ssel == 2) {
    // satp.ASID is 9 bits on RV32 (priv spec 4.1.11).
    if (((s->satp >> kSatpAsidShift) & kSatpAsidMask) !=
        ((t3 >> kTd3SvalueShift) & kSatpAsidMask))
      return 0;
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
static int TrigReentryOk(Riscv32State* s) {
  int bp_deleg = (int)((s->medeleg >> kExBreakpoint) & 1);
  if (s->priv == kPrivMachine && !(s->mstatus & kMstatusMie)) return 0;
  if (s->priv == kPrivSupervisor && bp_deleg && !(s->mstatus & kMstatusSie)) return 0;
  return 1;
}

// action 0 = breakpoint exception (mtval carries the matched address).
_Noreturn static void Fire(frame* fr, uint64_t addr) {
  raise_(fr, kExBreakpoint, addr);
}

// The first byte of [addr, addr+span) that matches, or ~0.
static uint64_t MatchSpan(Riscv32State* s, uint64_t t1, uint64_t t2, uint64_t t3, int op,
                          uint64_t addr, int span, int size) {
  for (int i = 0; i < span; i++) {
    uint64_t a = addr + (uint64_t)i;
    if (Mc6Match(s, t1, t2, t3, op, a, size)) return a;
  }
  return ~0ULL;
}

void Riscv32TriggerCheck(frame* fr, Riscv32State* s, int op, uint64_t addr, int size) {
  if (!TrigReentryOk(s)) return;
  int span = (op & kTrigOpExecute) ? 1 : (size > 0 ? size : 1);
  uint64_t m0 = MatchSpan(s, s->tdata1[0], s->tdata2[0], s->tdata3[0], op, addr, span, size);
  uint64_t m1 = MatchSpan(s, s->tdata1[1], s->tdata2[1], s->tdata3[1], op, addr, span, size);
  int hit0 = m0 != ~0ULL, hit1 = m1 != ~0ULL;
  int chain0 = (int)((s->tdata1[0] >> 11) & 1);
  if (hit0 && !chain0) {
    s->tdata1[0] |= kMc6Hit0;
    Fire(fr, m0);
  }
  if ((hit0 || !chain0) && hit1 && !((s->tdata1[1] >> 11) & 1)) {
    s->tdata1[1] |= kMc6Hit0;
    Fire(fr, m1);
  }
}

// tdata1 write (debug spec 5.3.12; xiangshanNEMU mcontrol6_checked_write):
// the type is fixed to mcontrol6, dmode can only be cleared from outside
// debug mode, select is WARL-0 (no data-value matching), only match EQ/GE/LT
// and action 0/1 are accepted (others WARL to EQ/0), the chain bit is kept
// only where a successor trigger exists, and the hit/size/uncertain fields
// read 0 after a write. tselect selects the trigger (Sdtrig).
void Riscv32TriggerWriteTdata1(Riscv32State* s, uint64_t val) {
  int idx = (int)s->tselect;
  uint64_t cur = s->tdata1[idx];
  uint64_t type = (val >> 28) & 0xf;
  uint64_t action = (val >> 12) & 0xf;
  if (type != 6 || action != 0) {
    s->tdata1[idx] = kTrigTypeDisabled;
    return;
  }
  uint64_t v = kTrigTypeMcontrol6;
  v |= cur & val & kMc6Dmode;
  uint64_t match = (val >> 7) & 0xf;
  if (match == kMc6MatchEq || match == kMc6MatchGe || match == kMc6MatchLt) v |= match << 7;
  // Chain is stored as written: chain=1 means "wait for the successor".
  v |= val & kMc6Chain;
  v |= val & (kMc6Load | kMc6Store | kMc6Execute | kMc6U | kMc6S | kMc6M);
  uint64_t sz = (val >> 16) & 7;
  if (sz <= 4) v |= sz << 16;
  v |= val & (kMc6Hit0 | kMc6Hit1);
  s->tdata1[idx] = v;
}

// tdata3 write (debug spec 5.3.10): tdata3 is XLEN bits, so only the low 32
// bits exist on RV32 — the mhselect/mhvalue fields (bits 63:48) are absent
// and the sbytemask bits (39:36) are beyond the register and read 0. The
// Sscontext fields store as written (the match above limits which sselect
// encodings can ever fire). tselect selects the trigger.
void Riscv32TriggerWriteTdata3(Riscv32State* s, uint64_t val) {
  // RV32 Trigger_Extra: sselect 1:0, svalue 17:2, sbytemask 19:18; the mh
  // fields (31:23) read zero without mcontext matching.
  uint64_t v = val & 0xfffffULL;
  if ((v & 3) == 3) v &= ~3ULL;  // sselect=3 reserved
  s->tdata3[s->tselect] = v;
}

// ---- gdb stub watchpoints ---------------------------------------------------
//
// RSP Z2/Z3/Z4 name a write/read/access watchpoint at a virtual address and
// length (GDB Remote Serial Protocol, "Watchpoint Packets").  RISC-V
// mcontrol6 expresses the same three classes with its store/load bits (debug
// spec 5.3.12) and the address match in tdata2; QEMU nevertheless keeps gdb
// watchpoints in a separate CPU watchpoint list (system/watchpoint.c,
// cpu_watchpoint_insert) instead of reprogramming the guest's tdata CSRs, so
// the guest's two triggers are untouched.  This is that same split: the
// match semantics are mcontrol6's load/store classes, the storage is private
// to the stub.  A hit is reported to the stub after the step and is never
// vectored, because a bare guest has no breakpoint handler (cpu/isa/x86/step.c
// documents the same rule for DR0-DR7).

// True when the RSP kind selects this operation class: 2 = write (store),
// 3 = read (load), 4 = access (either).
static int GdbWatchKindMatch(int kind, int op) {
  switch (kind) {
    case 2:
      return (op & kTrigOpStore) != 0;
    case 3:
      return (op & kTrigOpLoad) != 0;
    case 4:
      return (op & (kTrigOpLoad | kTrigOpStore)) != 0;
    default:
      return 0;
  }
}

// Record a stub watchpoint hit for one memory operation.  The access is
// [addr, addr+size); the watch range is [base, base+len).  Any overlap is a
// hit, the same rule as x86's WatchData (cpu/isa/x86/exec.c) and QEMU's
// watchpoint range check: a wider access that covers one watched byte counts.
void Riscv32GdbWatchCheck(Riscv32State* s, int op, uint64_t addr, int size) {
  if (size <= 0) return;
  uint64_t a_last = addr + (uint64_t)size - 1;
  if (a_last < addr) return;  // an access that wraps the address space
  for (int i = 0; i < kGdbWatchCount; i++) {
    if (!(s->gdb_watch_mask & (1u << i)) ||
        !GdbWatchKindMatch(s->gdb_watch_kind[i], op))
      continue;
    uint64_t w_lo = s->gdb_watch_addr[i];
    uint64_t w_last = w_lo + (uint64_t)s->gdb_watch_len[i] - 1;
    if (addr <= w_last && w_lo <= a_last) s->gdb_watch_hits |= 1u << i;
  }
}

// Arm one stub watchpoint.  The RSP length is an exact byte range; mcontrol6's
// size field names the common 1/2/4/8 sizes, but this matcher (like QEMU's
// CPU watchpoint list, system/watchpoint.c) keeps the range gdb asked for.
// RV32 addresses are 32 bits; a request outside that space can never match and
// is refused rather than silently truncated.  The overflow rule is QEMU's
// cpu_watchpoint_insert: an empty range or one that runs off the end is invalid.
int Riscv32GdbSetWatch(CpuState* cpu, uint64_t addr, uint32_t len, int kind) {
  Riscv32State* s = (Riscv32State*)cpu->priv;
  if (kind < 2 || kind > 4) return -1;
  if (len == 0) return -1;
  if (addr > kXlenMask || addr + (uint64_t)len - 1 > kXlenMask) return -1;
  for (int i = 0; i < kGdbWatchCount; i++) {
    if (s->gdb_watch_mask & (1u << i)) continue;
    s->gdb_watch_mask |= 1u << i;
    s->gdb_watch_kind[i] = (uint8_t)kind;
    s->gdb_watch_addr[i] = addr;
    s->gdb_watch_len[i] = len;
    s->gdb_watch_hits &= ~(1u << i);
    return i;
  }
  return -1;  // all four slots are in use
}

// Disarm a stub watchpoint.  The slot number is the one gdb_set_watch
// returned; a guest trigger was never touched, so there is nothing else to
// restore.
int Riscv32GdbClearWatch(CpuState* cpu, int slot) {
  Riscv32State* s = (Riscv32State*)cpu->priv;
  if (slot < 0 || slot >= kGdbWatchCount ||
      !(s->gdb_watch_mask & (1u << slot)))
    return -1;
  s->gdb_watch_mask &= ~(1u << slot);
  s->gdb_watch_hits &= ~(1u << slot);
  return 0;
}

// The slots that fired on the step just committed (cpu/step.h gdb_watch_hits).
// step_body.inc clears the mask at the start of every step, so a hit is
// reported exactly once.
uint32_t Riscv32GdbWatchHits(CpuState* cpu) {
  return ((Riscv32State*)cpu->priv)->gdb_watch_hits;
}
