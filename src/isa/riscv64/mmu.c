// Sv39 address translation and PMP enforcement. Semantics translated from
// xiangshanNEMU src/isa/riscv64/system/mmu.c (ptw, check_permission,
// pmp_check_permission_with_mode); divergences follow the priv spec or are
// calibrated against QEMU behavior where noted. The interpreter walks page
// tables on every access — there is no translation cache, so sfence.vma has
// nothing to flush beyond its permission checks.
#include "riscv.h"

// Sv39 PTE fields (priv spec 4.3.2).
enum {
  kPteV = 1 << 0,
  kPteR = 1 << 1,
  kPteW = 1 << 2,
  kPteX = 1 << 3,
  kPteU = 1 << 4,
  kPteG = 1 << 5,
  kPteA = 1 << 6,
  kPteD = 1 << 7,
};
// Bits [63:54] hold N/pbmt/reserved fields; Svpbmt and Svnapot are not
// implemented, so any of them set is a page fault (priv spec 4.3.2).
static const uint64_t kPteReserved = ~((1ULL << 54) - 1);

// ---- PMP (priv spec 3.7) ----

static uint8_t PmpCfg(const RiscvState *s, int i) {
  return (uint8_t)(s->pmpcfg[i / 8] >> (8 * (i % 8)));
}

// Decodes entry i's byte range; returns 0 for inactive (A=OFF) or empty
// ranges. NA4/NAPOT follow the raw pmpaddr encoding (QEMU
// target/riscv/pmp.c pmp_napot_get_range); TOR entries read pmpaddr[i-1]
// as the lower bound and mask both to the 16-byte grain (G=2).
static int PmpRegion(const RiscvState *s, int i, uint64_t *base,
                     uint64_t *size) {
  uint8_t cfg = PmpCfg(s, i);
  uint64_t a = cfg & kPmpAMask;
  uint64_t addr = s->pmpaddr[i];
  if (a == kPmpATor) {
    uint64_t lo = i > 0 ? (s->pmpaddr[i - 1] & ~3ULL) : 0;
    uint64_t hi = addr & ~3ULL;
    if (hi <= lo) return 0;  // empty TOR range matches nothing
    *base = lo << 2;
    *size = (hi - lo) << 2;
    return 1;
  }
  if (a == kPmpANa4) {
    *base = addr << 2;
    *size = 4;
    return 1;
  }
  if (a == kPmpANapot) {
    uint64_t inv = ~addr;
    if (inv == 0) {  // all ones: the whole address space
      *base = 0;
      *size = ~0ULL;
      return 1;
    }
    int n = 0;  // count of trailing ones in pmpaddr
    while (!(inv >> n & 1)) n++;
    *base = (addr >> n) << (n + 2);
    *size = 1ULL << (n + 2);
    return 1;
  }
  return 0;  // A = OFF
}

int RiscvPmpAllowed(const RiscvState *s, uint64_t addr, uint64_t len, int acc,
                    int mode) {
  int need_r = (acc == kAccRead || acc == kAccAmo);
  int need_w = (acc == kAccWrite || acc == kAccAmo);
  int need_x = (acc == kAccIfetch);
  for (int i = 0; i < kPmpCount; i++) {
    uint64_t base, size;
    if (!PmpRegion(s, i, &base, &size)) continue;
    int any = addr < base + size && addr + len > base;
    if (!any) continue;
    // The lowest-numbered matching entry decides; an access that straddles
    // an entry boundary is denied (xiangshanNEMU pmp_check_permission).
    if (!(addr >= base && addr + len <= base + size)) return 0;
    uint8_t cfg = PmpCfg(s, i);
    if (mode == kPrivMachine && !(cfg & kPmpL)) return 1;  // M passes unlocked
    if ((need_r && !(cfg & kPmpR)) || (need_w && !(cfg & kPmpW)) ||
        (need_x && !(cfg & kPmpX)))
      return 0;
    return 1;
  }
  return mode == kPrivMachine;  // no match: M passes, S/U are denied
}

// ---- Sv39 (priv spec 4.3.2 / 5.3) ----

static int AccessFaultCause(int acc) {
  if (acc == kAccIfetch) return kExFetchFault;
  if (acc == kAccRead) return kExLoadFault;
  return kExStoreFault;
}

static int PageFaultCause(int acc) {
  if (acc == kAccIfetch) return kExFetchPageFault;
  if (acc == kAccRead) return kExLoadPageFault;
  return kExStorePageFault;
}

// PTE fetch: a physical 8-byte read checked as S-mode (xiangshanNEMU mmu.c
// walks page tables under MODE_S regardless of the trapped privilege).
static int PteRead(CpuState *cpu, RiscvState *s, uint64_t pte_addr, int acc,
                   uint64_t *out) {
  Bus *bus = cpu->bus;
  if (!RiscvPmpAllowed(s, pte_addr, 8, kAccRead, kPrivSupervisor) ||
      BusProbe(bus, pte_addr, 8, NULL) != 0) {
    RiscvTrap(cpu, s, AccessFaultCause(acc), pte_addr);
    return -1;
  }
  *out = BusRead(bus, pte_addr, 8);
  return 0;
}

int RiscvTranslate(CpuState *cpu, RiscvState *s, uint64_t vaddr, int acc,
                   uint64_t *paddr) {
  // Effective privilege for data accesses under MPRV uses MPP (priv spec
  // 3.1.6.6); instruction fetch always uses the current mode.
  int eff = s->priv;
  if ((s->mstatus & kMstatusMprv) && acc != kAccIfetch)
    eff = (int)((s->mstatus & kMstatusMppMask) >> 11);
  uint64_t satp_mode = (s->satp >> 60) & 0xf;
  if (eff == kPrivMachine || satp_mode == kSatpModeBare) {
    *paddr = vaddr;
    return 0;
  }

  // Sv39 canonical-address check (priv spec 5.3): bits 63:39 must all copy
  // bit 38.
  int64_t v39 = (int64_t)(vaddr << 25) >> 25;
  if ((uint64_t)v39 != vaddr) {
    RiscvTrap(cpu, s, PageFaultCause(acc), vaddr);
    return -1;
  }

  uint64_t ppn_base = s->satp & ((1ULL << 44) - 1);
  uint64_t pte = 0;
  uint64_t pte_addr = 0;
  int level;
  for (level = 2; level >= 0; level--) {
    uint64_t vpn = (vaddr >> (12 + 9 * level)) & 0x1ff;
    pte_addr = (ppn_base << 12) + vpn * 8;
    if (PteRead(cpu, s, pte_addr, acc, &pte) != 0) return -1;
    if (!(pte & kPteV) || (!(pte & kPteR) && (pte & kPteW)) ||
        (pte & kPteReserved)) {
      RiscvTrap(cpu, s, PageFaultCause(acc), vaddr);
      return -1;
    }
    if (pte & (kPteR | kPteX)) break;  // leaf
    // Non-leaf PTEs must have A/D/U clear (priv spec 4.3.2).
    if (pte & (kPteA | kPteD | kPteU)) {
      RiscvTrap(cpu, s, PageFaultCause(acc), vaddr);
      return -1;
    }
    ppn_base = pte >> 10;
  }
  if (level < 0) {  // walk ran out of levels
    RiscvTrap(cpu, s, PageFaultCause(acc), vaddr);
    return -1;
  }

  // Permission check (xiangshanNEMU check_permission): U pages from S need
  // SUM (never for fetch); U mode needs the U bit; MXR turns X-only pages
  // readable.
  int ok = 1;
  if (eff == kPrivUser && !(pte & kPteU)) ok = 0;
  if ((pte & kPteU) && eff == kPrivSupervisor &&
      (!(s->mstatus & kMstatusSum) || acc == kAccIfetch))
    ok = 0;
  if (acc == kAccIfetch) {
    if (!ok || !(pte & kPteX)) goto page_fault;
  } else if (acc == kAccRead) {
    if (!ok || (!((pte & kPteR) ||
                  ((s->mstatus & kMstatusMxr) && (pte & kPteX)))))
      goto page_fault;
  } else {  // write and AMO need W (the AMO read side faults as a store,
            // xiangshanNEMU uses EX_SPF for cpu.amo)
    if (!ok || !(pte & kPteW)) goto page_fault;
  }
  // A/D handling (Svadu, priv spec 4.3.1): with menvcfg.ADUE set the
  // hardware sets the bits and writes the PTE back (QEMU's default); with
  // ADUE clear the access faults instead (spike / xiangshanNEMU behavior).
  if (!(pte & kPteA) || (acc != kAccRead && !(pte & kPteD))) {
    if (!(s->menvcfg & kMenvcfgAdue)) goto page_fault;
    uint64_t new_pte = pte | kPteA;
    if (acc != kAccRead) new_pte |= kPteD;
    if (new_pte != pte) {
      // The PTE lives in RAM on every machine that pages; a missing region
      // means it no longer does, which is an access fault.
      if (BusProbe(cpu->bus, pte_addr, 8, NULL) != 0) {
        RiscvTrap(cpu, s, AccessFaultCause(acc), vaddr);
        return -1;
      }
      BusWrite(cpu->bus, pte_addr, 8, new_pte);
    }
    pte = new_pte;
  }

  uint64_t ppn = pte >> 10;
  if (level > 0) {
    // Superpage: PPN bits below the leaf level must be zero (priv spec
    // 4.3.2), which also keeps PPN[2] within 27 bits for Sv39.
    uint64_t low = (1ULL << (9 * level)) - 1;
    if (ppn & low) goto page_fault;
    uint64_t off_mask = (1ULL << (12 + 9 * level)) - 1;
    *paddr = ((ppn & ~low) << 12) | (vaddr & off_mask);
  } else {
    *paddr = (ppn << 12) | (vaddr & 0xfff);
  }
  return 0;

page_fault:
  RiscvTrap(cpu, s, PageFaultCause(acc), vaddr);
  return -1;
}
