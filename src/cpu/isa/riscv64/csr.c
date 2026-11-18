#include <stdio.h>
#include <stdlib.h>

#include "host/host.h"
#include "riscv.h"
#include "util/log.h"

// mcounteren gates S/U reads of the machine counters (priv spec 3.1.11).
enum { kCounterenCycle = 1 << 0, kCounterenTime = 1 << 1, kCounterenInstret = 1 << 2 };

static uint64_t MipAll(const RiscvState* s) { return s->mip | s->ext_irq; }

void riscv_set_ext_irq(CpuState* cpu, uint64_t bit, int level) {
  RiscvState* s = (RiscvState*)cpu->priv;
  if (level)
    s->ext_irq |= bit;
  else
    s->ext_irq &= ~bit;
}

// Traps route to S when the cause is delegated and the trap did not come
// from M mode (priv spec 3.5, xiangshanNEMU intr.c intr_deleg_S). Returns
// the vector base; the caller routes the pc (run-loop dnpc commit, or the
// between-instruction injection in DeliverPendingInterrupt).
uint64_t riscv_trap(CpuState* cpu, RiscvState* s, uint64_t cause, uint64_t tval) {
  int is_int = (cause & kIntBit) != 0;
  uint64_t deleg_src = is_int ? s->mideleg : s->medeleg;
  int to_s = ((deleg_src >> (cause & 0xfff)) & 1) && s->priv < kPrivMachine;
  if (to_s) {
    s->sepc = cpu->pc;
    s->scause = cause;
    s->stval = tval;
    uint64_t st = s->mstatus;
    st = (st & ~kMstatusSpie) | ((st & kMstatusSie) ? kMstatusSpie : 0);
    st &= ~kMstatusSie;
    st = (st & ~kMstatusSpp) | (s->priv == kPrivSupervisor ? kMstatusSpp : 0);
    s->mstatus = st;
    s->priv = kPrivSupervisor;
    uint64_t base = s->stvec & ~3ULL;
    if ((s->stvec & 3) == 1 && is_int) base += 4 * (cause & 0xf);
    return base;
  } else {
    s->mepc = cpu->pc;
    s->mcause = cause;
    s->mtval = tval;
    uint64_t st = s->mstatus;
    st = (st & ~kMstatusMpie) | ((st & kMstatusMie) ? kMstatusMpie : 0);
    st &= ~kMstatusMie;
    st = (st & ~kMstatusMppMask) | ((uint64_t)s->priv << 11);  // MPP <- interrupted privilege
    s->mstatus = st;
    s->priv = kPrivMachine;
    uint64_t base = s->mtvec & ~3ULL;
    if ((s->mtvec & 3) == 1 && is_int) base += 4 * (cause & 0xf);
    return base;
  }
}

void riscv_sync_timer(CpuState* cpu, RiscvState* s) {
  // Sstc: when the machine provides mtime, stimecmp drives the STIP line
  // (QEMU target/riscv does not gate the CSR on menvcfg.STCE). Skipped
  // while stimecmp holds its reset all-ones value so the common path costs
  // no host clock reads. step.c calls this on the idle cadence, every 1024
  // steps, and on every step while STIP is asserted (the comparator is a
  // level, so a stimecmp write must retire the request at once).
  // Sstc: stimecmp drives STIP only while menvcfg.STCE enables Sstc. With
  // STCE=0, STIP is the platform/software bit (priv spec 3.1.9; Sail
  // interrupt_regs.sail gates the comparator on STCE).
  if (!(s->menvcfg & kMenvcfgStce)) {
    riscv_set_ext_irq(cpu, kIrqStip, 0);
    return;
  }
  if (cpu->timer_read && s->stimecmp != ~0ULL)
    riscv_set_ext_irq(cpu, kIrqStip, cpu->timer_read(cpu->timer_dev) >= s->stimecmp);
}

void riscv_deliver_interrupt(CpuState* cpu, RiscvState* s) {
  uint64_t pending = MipAll(s) & s->mie;
  if (!pending) return;
  // Strict priority order (priv spec 3.1.9): MEI, MSI, MTI, SEI, SSI, STI.
  static const uint8_t kOrder[] = {11, 3, 7, 9, 1, 5};
  for (size_t i = 0; i < sizeof(kOrder) / sizeof(kOrder[0]); i++) {
    int id = kOrder[i];
    if (!((pending >> id) & 1)) continue;
    int delegated = (int)((s->mideleg >> id) & 1);
    int take;
    if (delegated) {
      // Targets S: gated by SIE only when trapping from S itself.
      take = (s->priv == kPrivSupervisor) ? ((s->mstatus & kMstatusSie) != 0)
                                          : (s->priv < kPrivSupervisor);
    } else {
      // Targets M: gated by MIE only when trapping from M itself.
      take =
          (s->priv == kPrivMachine) ? ((s->mstatus & kMstatusMie) != 0) : (s->priv < kPrivMachine);
    }
    if (take) {
      // Interrupt injection happens strictly between instructions, so this
      // is the one sanctioned direct cpu->pc write (the run loop commits
      // dnpc for instruction-driven control flow).
      cpu->pc = riscv_trap(cpu, s, kIntBit | (uint64_t)id, 0);
      return;
    }
  }
}

// Returns the mret target PC; the caller routes it through next_pc.
uint64_t riscv_mret(RiscvState* s) {
  uint64_t st = s->mstatus;
  st = (st & ~kMstatusMie) | ((st & kMstatusMpie) ? kMstatusMie : 0);
  st |= kMstatusMpie;
  uint64_t mpp = (st & kMstatusMppMask) >> 11;
  if (mpp != kPrivMachine) st &= ~kMstatusMprv;
  st &= ~kMstatusMppMask;  // MPP <- U
  s->mstatus = st;
  s->priv = (uint8_t)mpp;
  uint64_t target = s->mepc & ~1ULL;
  if (!(s->misa & kMisaC)) target &= ~2ULL;  // IALIGN=32 masks mepc[1]
  return target;
}

// Returns the sret target PC. The TSR check happens at the instruction
// decode site, which has the original instruction for the illegal trap.
uint64_t riscv_sret(RiscvState* s) {
  uint64_t st = s->mstatus;
  st = (st & ~kMstatusSie) | ((st & kMstatusSpie) ? kMstatusSie : 0);
  st |= kMstatusSpie;
  st &= ~kMstatusMprv;  // returning below M clears MPRV (priv spec 3.1.6.6)
  s->priv = (st & kMstatusSpp) ? kPrivSupervisor : kPrivUser;
  st &= ~kMstatusSpp;  // SPP <- U
  s->mstatus = st;
  uint64_t target = s->sepc & ~1ULL;
  if (!(s->misa & kMisaC)) target &= ~2ULL;
  return target;
}

// Counter CSRs. S/U reads are gated by mcounteren (and scounteren for
// U-mode, priv spec 3.1.11). The write records which counter it touched so
// Step can suppress that counter's increment for this instruction.
static int ReadCounter(const CpuState* cpu, RiscvState* s, uint64_t addr, uint64_t* out) {
  if (s->priv < kPrivMachine) {
    uint64_t gate = 0;
    switch (addr) {
      case 0xC00:
        gate = kCounterenCycle;
        break;
      case 0xC01:
        gate = kCounterenTime;
        break;
      case 0xC02:
        gate = kCounterenInstret;
        break;
      default:
        if (addr >= 0xC03 && addr <= 0xC1F)
          gate = 1ULL << (addr - 0xC00);  // hpmcounter3..31 aliases
        break;
    }
    if (!(s->mcounteren & gate)) return -1;
    if (s->priv == kPrivUser && !(s->scounteren & gate)) return -1;
  }
  switch (addr) {
    case 0xB00:
    case 0xC00:
      *out = s->mcycle;
      return 0;
    case 0xB02:
    case 0xC02:
      *out = s->minstret;
      return 0;
    case 0xC01:
      // time: the machine timer when the platform provides one (virt CLINT
      // mtime); otherwise the registered mcycle alias (the spike board has no
      // mtime, so mcycle stands in).
      *out = cpu->timer_read ? cpu->timer_read(cpu->timer_dev) : s->mcycle;
      return 0;
    default:
      if (addr >= 0xC03 && addr <= 0xC1F) {
        *out = s->mhpmcounter[addr - 0xC03];
        return 0;  // S/U hpmcounter alias (Zihpm)
      }
      return -1;
  }
}

// The minimum privilege to access a CSR is encoded in its address bits
// [9:8] (priv spec 2.2).
static int CsrPriv(uint64_t addr) { return (int)((addr >> 8) & 3); }

int riscv_csr_read(const CpuState* cpu, RiscvState* s, uint64_t addr, uint64_t* out) {
  if (CsrPriv(addr) > s->priv) return -1;
  // fflags/frm/fcsr exist only while mstatus.FS != 0 (unpriv spec 11.3;
  // Sail is_CSR_accessible for 0x001-0x003 requires currentlyEnabled(Ext_F)).
  if (addr >= 0x001 && addr <= 0x003 && !(s->mstatus & kMstatusFs)) return -1;
  if (ReadCounter(cpu, s, addr, out) == 0) return 0;
  switch (addr) {
    case 0x001:
      *out = s->fflags;
      return 0;
    case 0x002:
      *out = s->frm;
      return 0;
    case 0x003:
      *out = (uint64_t)s->frm << 5 | s->fflags;
      return 0;
    case 0x100: {  // sstatus view of mstatus (priv spec 3.1.6.3)
      uint64_t v = s->mstatus & kSstatusRmask;
      if ((s->mstatus & kMstatusFs) == kMstatusFs) v |= 1ULL << 63;  // SD
      *out = v;
      return 0;
    }
    case 0x104:
      // sie is the delegated half of mie (Sail read_CSR: lower_mie(mie, mideleg)).
      *out = s->mie & (s->mideleg & kIrqSLevelMask);
      return 0;  // sie view
    case 0x105:
      *out = s->stvec;
      return 0;
    case 0x106:
      *out = s->scounteren;
      return 0;
    case 0x10A:
      *out = s->senvcfg;
      return 0;
    case 0x14D:
      // Sstc is usable from S mode only with menvcfg.STCE and mcounteren.TM
      // (Sail sstc.sail).
      if (s->priv == kPrivSupervisor &&
          (!(s->menvcfg & kMenvcfgStce) || !(s->mcounteren & (1ULL << 1))))
        return -1;
      *out = s->stimecmp;
      return 0;
    case 0x140:
      *out = s->sscratch;
      return 0;
    case 0x141:
      *out = (s->misa & kMisaC) ? s->sepc : s->sepc & ~2ULL;  // E3: IALIGN
      return 0;
    case 0x142:
      *out = s->scause;
      return 0;
    case 0x143:
      *out = s->stval;
      return 0;
    case 0x144:
      *out = MipAll(s) & (s->mideleg & kIrqSLevelMask);
      return 0;  // sip view
    case 0x180:
      // TVM gates S-mode satp reads too (priv spec 3.1.6.8).
      if (s->priv == kPrivSupervisor && (s->mstatus & kMstatusTvm)) return -1;
      *out = s->satp;
      return 0;
    case 0x300: {
      // SD reflects FS/VS/XS being Dirty; SXL mirrors MXL on RV64 (Sail
      // read_mstatus: SD = dirty, SXL = mxl when S is implemented).
      uint64_t v = s->mstatus;
      if ((v & kMstatusFs) == kMstatusFs) v |= 1ULL << 63;
      v |= 2ULL << 34;  // SXL = 0b10
      *out = v;
      return 0;
    }
    case 0x301:
      *out = s->misa;
      return 0;
    case 0x302:
      *out = s->medeleg;
      return 0;
    case 0x303:
      *out = s->mideleg;
      return 0;
    case 0x304:
      *out = s->mie & kIrqImplementedMask;
      return 0;
    case 0x305:
      *out = s->mtvec;
      return 0;
    case 0x306:
      *out = s->mcounteren;
      return 0;
    case 0x30A:
      *out = s->menvcfg;
      return 0;
    case 0x320:
      *out = s->mcountinhibit & (kMcountinhibitCy | kMcountinhibitIr | kMcountinhibitHpm);
      return 0;
    case 0x340:
      *out = s->mscratch;
      return 0;
    case 0x341:
      *out = (s->misa & kMisaC) ? s->mepc : s->mepc & ~2ULL;  // E3: IALIGN
      return 0;
    case 0x342:
      *out = s->mcause;
      return 0;
    case 0x343:
      *out = s->mtval;
      return 0;
    case 0x344:
      *out = MipAll(s);
      return 0;
    case 0x5A8:
      *out = s->scontext & 0xffffffffULL;
      return 0;  // scontext (Sdtrig; 32 bits wide on RV64)
    case 0x7A8:
      *out = s->mcontext & 0x3fffULL;
      return 0;  // mcontext (Sdtrig; 14 bits)
    case 0x7A0:
      *out = s->tselect;
      return 0;  // tselect
    case 0x7A1:
      *out = s->tdata1[s->tselect];
      return 0;  // tdata1 of the tselect-selected trigger
    case 0x7A2:
      *out = s->tdata2[s->tselect];
      return 0;  // tdata2
    case 0x7A3:
      *out = s->tdata3[s->tselect];
      return 0;  // tdata3
    case 0x7A4:
      // tinfo: version 1.0, mcontrol6 present, disabled type supported.
      *out = (1ULL << 24) | (1ULL << 6) | (1ULL << 15);
      return 0;
    case 0x7A5:
      *out = s->tcontrol;
      return 0;  // tcontrol
    case 0xF11:
      *out = 0;
      return 0;  // mvendorid
    case 0xF12:
      *out = 0;
      return 0;  // marchid
    case 0xF13:
      *out = 0;
      return 0;  // mimpid
    case 0xF14:
      *out = 0;
      return 0;  // mhartid
    case 0xF15:
      *out = 0;
      return 0;  // mconfigptr (priv spec 1.12+; no config structure)
    default:
      // MHPM counters/events 3..18 (Zihpm, the DTB-advertised set)
      if (addr >= 0xB03 && addr <= 0xB03 + kMhpmCount - 1) {
        *out = s->mhpmcounter[addr - 0xB03];
        return 0;
      }
      if (addr >= 0x323 && addr <= 0x323 + kMhpmCount - 1) {
        *out = s->mhpmevent[addr - 0x323] & ((1ULL << 56) - 1);  // no Sscofpmf
        return 0;
      }
      // RV64 packs 8 cfg entries into the even pmpcfg CSRs (priv spec 3.7.1)
      if (addr >= 0x3A0 && addr <= 0x3A2 && (addr & 1) == 0) {
        *out = s->pmpcfg[(addr - 0x3A0) / 2];
        return 0;
      }
      if (addr >= 0x3B0 && addr <= 0x3BF) {
        int i = (int)(addr - 0x3B0);
        uint8_t cfg = (uint8_t)(s->pmpcfg[i / 8] >> (8 * (i % 8)));
        uint64_t a = s->pmpaddr[i];
        // G=2: NAPOT's low bit reads as one, TOR/OFF's low two bits as zero
        // (Sail pmp_regs.sail pmpReadAddrReg).
        *out = (cfg & 0x10) ? (a | 1) : (a & ~3ULL);
        return 0;
      }
      return -1;
  }
}

// PMP write paths (priv spec 3.7, xiangshanNEMU priv.c pmpcfg/pmpaddr WARL;
// QEMU-calibrated parameters: 16 entries, G=2, 54 address bits).
static void WritePmpCfg(RiscvState* s, int which, uint64_t val) {
  uint64_t out = 0;
  for (int b = 0; b < 8; b++) {
    uint8_t old = (uint8_t)(s->pmpcfg[which] >> (8 * b));
    if (old & kPmpL) {  // locked entries are immutable
      out |= (uint64_t)old << (8 * b);
      continue;
    }
    uint8_t cfg = (uint8_t)(val >> (8 * b)) & 0x9f;  // bits 6:5 read-only zero
    if (!(cfg & kPmpR)) cfg &= (uint8_t)~kPmpW;  // W requires R
    if ((cfg & kPmpAMask) == kPmpANa4)
      cfg = (uint8_t)((cfg & ~kPmpAMask) | kPmpANapot);  // G=2: NA4 -> NAPOT
    out |= (uint64_t)cfg << (8 * b);
  }
  s->pmpcfg[which] = out;
  s->pmp_gen++;
}

static void WritePmpAddr(RiscvState* s, int i, uint64_t val) {
  if (i >= kPmpCount) return;
  uint8_t cfg = (uint8_t)(s->pmpcfg[i / 8] >> (8 * (i % 8)));
  uint8_t next_cfg =
      (i + 1 < kPmpCount) ? (uint8_t)(s->pmpcfg[(i + 1) / 8] >> (8 * ((i + 1) % 8))) : 0;
  // A locked entry's address is immutable, as is any entry whose successor
  // is locked and TOR (it forms the successor's lower bound).
  if ((cfg & kPmpL) || ((next_cfg & kPmpL) && (next_cfg & kPmpAMask) == kPmpATor)) return;
  s->pmpaddr[i] = val & ((1ULL << kPmpAddrBits) - 1);
  s->pmp_gen++;
}

int riscv_csr_write(RiscvState* s, uint64_t addr, uint64_t val, uint64_t next_pc) {
  if (CsrPriv(addr) > s->priv) return -1;
  // fflags/frm/fcsr exist only while mstatus.FS != 0 (unpriv spec 11.3).
  if (addr >= 0x001 && addr <= 0x003 && !(s->mstatus & kMstatusFs)) return -1;
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
    case 0x001:
      s->fflags = (uint8_t)(val & 0x1f);
      return 0;
    case 0x002:
      s->frm = (uint8_t)(val & 7);
      return 0;
    case 0x003:
      s->frm = (uint8_t)((val >> 5) & 7);
      s->fflags = (uint8_t)(val & 0x1f);
      return 0;
    case 0x100:  // sstatus view: only the S-level fields of mstatus
      s->mstatus = (s->mstatus & ~kSstatusWmask) | (val & kSstatusWmask);
      return 0;
    case 0x104: {  // sie view: only delegated interrupt bits are visible/writable
      uint64_t mask = s->mideleg & kIrqSLevelMask;
      s->mie = (s->mie & ~mask) | (val & mask);
      return 0;
    }
    case 0x105: {
      // Reserved MODE values keep the previous mode (Sail xtvec_mode_ignore)
      // instead of folding 2/3 into Direct/Vectored.
      uint64_t mode = val & 3;
      s->stvec = (val & ~3ULL) | (mode <= 1 ? mode : (s->stvec & 3));
      return 0;
    }
    case 0x106:
      s->scounteren = val & 0xffffffffULL;
      return 0;
    case 0x10A: {
      uint64_t v = val & kSenvcfgWmask;
      if ((v & kMenvcfgCbie) == 0x20) v &= ~kMenvcfgCbie;  // CBIE=0b10 reserved
      s->senvcfg = v;
      return 0;
    }
    case 0x14D:
      if (s->priv == kPrivSupervisor &&
          (!(s->menvcfg & kMenvcfgStce) || !(s->mcounteren & (1ULL << 1))))
        return -1;
      s->stimecmp = val;
      return 0;  // Sstc: retires a pending STIP
    case 0x140:
      s->sscratch = val;
      return 0;
    case 0x141:
      s->sepc = val & ~1ULL;
      return 0;
    case 0x142:
      s->scause = val;
      return 0;
    case 0x143:
      s->stval = val;
      return 0;
    case 0x144: {  // sip view: only delegated SSIP is writable (priv spec 3.1.9)
      uint64_t mask = kIrqSsip & s->mideleg;
      s->mip = (s->mip & ~mask) | (val & mask);
      return 0;
    }
    case 0x180: {  // satp
      // S-mode writes with TVM set trap (priv spec 3.1.6.8, 4.1.11).
      if (s->priv == kPrivSupervisor && (s->mstatus & kMstatusTvm)) return -1;
      uint64_t mode = (val >> 60) & 0xf;
      // WARL: only Bare, Sv39 and Sv48 are implemented; other modes leave
      // satp unchanged (xiangshanNEMU priv.c CSR_SATP write path; Sv48 per
      // priv spec 4.4.1).
      if (mode != kSatpModeBare && mode != kSatpModeSv39 && mode != kSatpModeSv48)
        return 0;
      s->satp = val & kSatpWmask;
      // The reservation is keyed by virtual address; a new address space makes
      // the old pair meaningless. SC may fail spuriously, so dropping it here
      // is legal (Sail keys reservations by physical address instead).
      s->res_valid = 0;
      return 0;
    }
    case 0x300: {  // mstatus
      // WARL fields (priv spec 3.1.6): UXL stays 0b10 (only RV64 U-mode
      // exists); MPP holds only implemented modes (U/S/M) and clears when
      // set to the unimplemented H mode.
      uint64_t mpp = val & kMstatusMppMask;
      if (mpp == (uint64_t)kPrivHypervisor << 11) val &= ~kMstatusMppMask;
      s->mstatus = (s->mstatus & ~kMstatusWmask) | (val & kMstatusWmask);
      s->mstatus = (s->mstatus & ~kMstatusUxlMask) | kMstatusUxlValue;
      return 0;
    }
    case 0x301: {
      // WARL (priv spec 3.1.1): only bits whose clearing the hart honors are
      // writable; MXL and the extensions with no disable path are read-only.
      // Clearing C is rejected when the next fetch would violate IALIGN=32
      // (gem5 isa.cc implements the same check).
      uint64_t new_misa = (val & kMisaWritable) | (kMisaSupported & ~kMisaWritable);
      if ((s->misa & kMisaC) && !(new_misa & kMisaC) && (next_pc % 4)) new_misa |= kMisaC;
      s->misa = new_misa;
      return 0;
    }
    case 0x302:
      s->medeleg = val & kMedelegWmask;
      return 0;
    case 0x303:
      s->mideleg = val & kMidelegWmask;
      return 0;
    case 0x304:
      s->mie = val & kIrqImplementedMask;
      return 0;
    case 0x305: {
      uint64_t mode = val & 3;
      s->mtvec = (val & ~3ULL) | (mode <= 1 ? mode : (s->mtvec & 3));
      return 0;
    }
    case 0x306:
      s->mcounteren = val & 0xffffffffULL;
      return 0;
    case 0x30A: {
      uint64_t v = val & kMenvcfgWmask;
      if ((v & kMenvcfgCbie) == 0x20) v &= ~kMenvcfgCbie;  // CBIE=0b10 reserved
      s->menvcfg = v;
      return 0;
    }
    case 0x320:
      s->mcountinhibit = val & (kMcountinhibitCy | kMcountinhibitIr | kMcountinhibitHpm);
      return 0;
    case 0x340:
      s->mscratch = val;
      return 0;
    case 0x341:
      s->mepc = val & ~1ULL;
      return 0;
    case 0x342:
      s->mcause = val;
      return 0;
    case 0x343:
      s->mtval = val;
      return 0;
    case 0x344: {  // mip: M-mode software forwarding of S/U interrupts
      // OpenSBI's timer and IPI forwarding is built on M-mode writing
      // mip.STIP/SSIP/SEIP (lib/sbi/sbi_timer.c csr_set(CSR_MIP, MIP_STIP)).
      // The spec leaves these bits to the platform (3.1.9); QEMU made them
      // M-mode-writable for exactly this firmware (target/riscv csr.c mip
      // mask), so the machine follows the de-facto contract. S-mode keeps
      // the sip view below, where only SSIP is software-writable.
      // With Sstc enabled (menvcfg.STCE=1) STIP is driven by stimecmp and
      // becomes read-only to software (Sail interrupt_regs.sail write_mip).
      uint64_t wmask = kIrqSsip | kIrqSeip | ((s->menvcfg & kMenvcfgStce) ? 0 : kIrqStip);
      s->mip = (s->mip & ~wmask) | (val & wmask);
      return 0;
    }
    case 0x5A8:
      s->scontext = val & 0xffffffffULL;
      return 0;  // scontext (Sdtrig; 32 bits wide on RV64)
    case 0x7A8:
      s->mcontext = val & 0x3fffULL;
      return 0;  // mcontext (Sdtrig; 14 bits)
    case 0x7A4:
      return 0;  // tinfo: write ignored (Sail sdtrig_regs.sail)
    case 0x7A0:
      // tselect WARL-clamps to the implemented trigger count (Sdtrig).
      s->tselect = val < kTrigCount ? val : kTrigCount - 1;
      return 0;
    case 0x7A1:
      RiscvTriggerWriteTdata1(s, val);  // mcontrol6 WARL policy (trigger.c)
      return 0;
    case 0x7A2:
      s->tdata2[s->tselect] = val;
      return 0;  // tdata2 of the tselect-selected trigger
    case 0x7A3:
      RiscvTriggerWriteTdata3(s, val);  // mh fields WARL-0 without H
      return 0;
    case 0x7A5:
      s->tcontrol = val & (kTcontrolMte | kTcontrolMpte);
      return 0;
    default:
      // MHPM counters/events 3..18 (Zihpm)
      if (addr >= 0xB03 && addr <= 0xB03 + kMhpmCount - 1) {
        s->mhpmcounter[addr - 0xB03] = val;
        return 0;
      }
      if (addr >= 0x323 && addr <= 0x323 + kMhpmCount - 1) {
        s->mhpmevent[addr - 0x323] = val & ((1ULL << 56) - 1);  // no Sscofpmf
        return 0;
      }
      if (addr >= 0x3A0 && addr <= 0x3A2 && (addr & 1) == 0) {
        WritePmpCfg(s, (int)((addr - 0x3A0) / 2), val);
        return 0;
      }
      if (addr >= 0x3B0 && addr <= 0x3BF) {
        WritePmpAddr(s, (int)(addr - 0x3B0), val);
        return 0;
      }
      return -1;  // read-only or unknown CSRs trap as illegal
  }
}
