#include <stdio.h>
#include <stdlib.h>
#include "riscv.h"
#include "host/host.h"
#include "util/log.h"

static const uint64_t kIntBit = 0x8000000000000000ULL;

enum { kPrivMachine = 3 };

void RiscvTrap(CpuState *cpu, RiscvState *s, uint64_t cause, uint64_t tval) {
  if (getenv("CEMU_TRACE")) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "[trap] pc=%016llx cause=%llu tval=%016llx\n",
                     (unsigned long long)cpu->pc, (unsigned long long)cause,
                     (unsigned long long)tval);
    HostWriteErr(buf, (size_t)n);
  }
  s->mepc = cpu->pc;
  s->mcause = cause;
  s->mtval = tval;
  uint64_t st = s->mstatus;
  st = (st & ~kMstatusMpie) | ((st & kMstatusMie) ? kMstatusMpie : 0);  // MPIE <- MIE
  st &= ~kMstatusMie;                                                   // MIE <- 0
  st = (st & ~kMstatusMppMask) | kMstatusMppMask;                       // MPP <- M
  s->mstatus = st;
  uint64_t base = s->mtvec & ~1ULL;
  if ((s->mtvec & 3) == 1 && (cause & kIntBit))
    base += 4 * (cause & 0x7fffffffULL);
  cpu->pc = base;
}

void RiscvDeliverPendingInterrupt(CpuState *cpu, RiscvState *s) {
  if (!(s->mstatus & kMstatusMie)) return;
  uint64_t pending = s->mip & s->mie;
  if (!pending) return;
  for (int bit = 15; bit >= 0; bit--) {
    if ((pending >> bit) & 1) {
      RiscvTrap(cpu, s, kIntBit | (uint64_t)bit, 0);
      return;
    }
  }
}

// Returns the mret target PC; the caller routes it through next_pc.
uint64_t RiscvMret(RiscvState *s) {
  uint64_t st = s->mstatus;
  st = (st & ~kMstatusMie) | ((st & kMstatusMpie) ? kMstatusMie : 0);  // MIE <- MPIE
  st |= kMstatusMpie;                                                  // MPIE <- 1
  st &= ~kMstatusMppMask;                                              // MPP <- U
  s->mstatus = st;
  s->priv = kPrivMachine;  // U mode is not implemented yet
  uint64_t target = s->mepc & ~1ULL;
  if (!(s->misa & kMisaC)) target &= ~2ULL;  // IALIGN=32 masks mepc[1]
  return target;
}

// Counter CSRs are RW in M mode. The write records which counter it touched
// so Step can suppress that counter's increment for this instruction.
static int ReadCounter(RiscvState *s, uint64_t addr, uint64_t *out) {
  switch (addr) {
    case 0xB00: case 0xC00: *out = s->mcycle; return 0;
    case 0xB02: case 0xC02: *out = s->minstret; return 0;
    case 0xC01: *out = s->mcycle; return 0;  // time: registered simplification D4
    default: return -1;
  }
}

int RiscvCsrRead(RiscvState *s, uint64_t addr, uint64_t *out) {
  if (ReadCounter(s, addr, out) == 0) return 0;
  // CSR addresses follow the priv spec CSR address map; bit-field constants
  // live in riscv.h.
  switch (addr) {
    case 0x001: *out = s->fflags; return 0;
    case 0x002: *out = s->frm; return 0;
    case 0x003: *out = (uint64_t)s->frm << 5 | s->fflags; return 0;
    case 0x100:
      // sstatus view: UXL hardwired to 2 (SXLEN=64)
      *out = s->mstatus | kMstatusUxlValue;
      return 0;
    case 0x105: *out = s->stvec; return 0;
    case 0x140: *out = s->sscratch; return 0;
    case 0x141: *out = s->sepc; return 0;
    case 0x142: *out = s->scause; return 0;
    case 0x143: *out = s->stval; return 0;
    case 0x144: *out = s->mip; return 0;  // sip view
    case 0x180: *out = s->satp; return 0;
    case 0x300: *out = s->mstatus; return 0;
    case 0x301: *out = s->misa; return 0;
    case 0x302: *out = s->medeleg; return 0;
    case 0x303: *out = s->mideleg; return 0;
    case 0x304: *out = s->mie; return 0;
    case 0x305: *out = s->mtvec; return 0;
    case 0x306: *out = s->mcounteren; return 0;
    case 0x340: *out = s->mscratch; return 0;
    case 0x341: *out = s->mepc; return 0;
    case 0x342: *out = s->mcause; return 0;
    case 0x343: *out = s->mtval; return 0;
    case 0x344: *out = s->mip; return 0;
    case 0x320: *out = s->mcountinhibit; return 0;
    case 0x7A0: *out = s->tselect; return 0;       // tselect
    case 0x7A1: *out = 0; return 0;                // tdata1: no triggers
    case 0x7A2: *out = 0; return 0;                // tdata2
    case 0x7A3: *out = 0; return 0;                // tdata3
    case 0x7A5: *out = s->tcontrol; return 0;      // tcontrol
    case 0xF11: *out = 0; return 0;  // mvendorid
    case 0xF12: *out = 0; return 0;  // marchid
    case 0xF13: *out = 0; return 0;  // mimpid
    case 0xF14: *out = 0; return 0;  // mhartid
    default:
      if (addr >= 0x3A0 && addr <= 0x3AF) {
        *out = s->pmpcfg[addr - 0x3A0];
        return 0;
      }
      if (addr >= 0x3B0 && addr <= 0x3EF) {
        *out = s->pmpaddr[addr - 0x3B0];
        return 0;
      }
      return -1;
  }
}

int RiscvCsrWrite(RiscvState *s, uint64_t addr, uint64_t val, uint64_t next_pc) {
  if (addr == 0xB00 || addr == 0xB02) {
    if (addr == 0xB00) {
      s->mcycle = val;
      s->counter_written = kCounterMcycle;
    } else {
      s->minstret = val;
      s->counter_written = kCounterMinstret;
    }
    return 0;
  }
  switch (addr) {
    case 0x001: s->fflags = (uint8_t)(val & 0x1f); return 0;
    case 0x002: s->frm = (uint8_t)(val & 7); return 0;
    case 0x003:
      s->frm = (uint8_t)((val >> 5) & 7);
      s->fflags = (uint8_t)(val & 0x1f);
      return 0;
    case 0x100:
      s->mstatus = (val & ~kMstatusUxlMask) | kMstatusUxlValue;  // sstatus view
      return 0;
    case 0x105: s->stvec = val & ~3ULL; return 0;
    case 0x140: s->sscratch = val; return 0;
    case 0x141: s->sepc = val & ~1ULL; return 0;
    case 0x142: s->scause = val; return 0;
    case 0x143: s->stval = val; return 0;
    case 0x144: s->mip = val; return 0;  // sip view
    case 0x180: s->satp = val; return 0;
    case 0x300:
      // WARL fields (priv spec 3.1.6): UXL stays 0b10 (only RV64 U-mode
      // exists); MPP holds only M and U because S and H are unimplemented,
      // so those writes clear to U.
      {
        uint64_t mpp = val & kMstatusMppMask;
        if (mpp != 0 && mpp != kMstatusMppMask) val &= ~kMstatusMppMask;
        s->mstatus = (val & ~kMstatusUxlMask) | kMstatusUxlValue;
      }
      return 0;
    case 0x301:
      // WARL (priv spec 3.1.10): only implemented extensions stick, I is
      // always set. Clearing C is rejected when the next fetch would violate
      // IALIGN=32 (gem5 isa.cc implements the same check).
      {
        uint64_t new_misa = (val & kMisaSupported) | kMisaI;
        if ((s->misa & kMisaC) && !(new_misa & kMisaC) && (next_pc % 4))
          new_misa |= kMisaC;
        s->misa = new_misa;
      }
      return 0;
    case 0x302: s->medeleg = val; return 0;
    case 0x303: s->mideleg = val; return 0;
    case 0x304: s->mie = val; return 0;
    case 0x305: s->mtvec = val & ~1ULL; return 0;  // 2-byte aligned (C present)
    case 0x306: s->mcounteren = val; return 0;
    case 0x340: s->mscratch = val; return 0;
    case 0x341: s->mepc = val & ~1ULL; return 0;
    case 0x342: s->mcause = val; return 0;
    case 0x343: s->mtval = val; return 0;
    case 0x344: s->mip = val; return 0;
    case 0x320: s->mcountinhibit = val; return 0;
    case 0x7A0: s->tselect = val; return 0;        // tselect
    case 0x7A1: return 0;                          // tdata1: WARL 0
    case 0x7A2: return 0;                          // tdata2: WARL 0
    case 0x7A3: return 0;                          // tdata3: WARL 0
    case 0x7A5: s->tcontrol = val & (kTcontrolMte | kTcontrolMpte); return 0;
    default:
      if (addr >= 0x3A0 && addr <= 0x3AF) {
        s->pmpcfg[addr - 0x3A0] = val;
        return 0;
      }
      if (addr >= 0x3B0 && addr <= 0x3EF) {
        s->pmpaddr[addr - 0x3B0] = val;
        return 0;
      }
      return -1;  // read-only or unknown CSRs trap as illegal
  }
}
