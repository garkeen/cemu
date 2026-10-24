// Sv32 address translation and PMP enforcement. The Sv39 walk in
// cpu/isa/riscv64/mmu.c translated to RV32's two-level format (priv spec
// 4.3.2 / 5.3): 4-byte PTEs, 10-bit VPNs, a 22-bit PPN in satp, no
// canonical-address check (the whole 32-bit VA space is valid) and no
// reserved high PTE bits. PMP keeps the reference's lowest-match-wins rule
// with the RV32 CSR layout (four entries per pmpcfg, 32-bit pmpaddr).
//
// The interpreter walks page tables on every access — there is no translation
// cache, so sfence.vma has nothing to flush beyond its permission checks.
//
// Faults raise through the frame (raise_): translation failures page-fault,
// PMP and unmapped-physical failures access-fault, each with the faulting
// virtual address in tval.
#include "riscv32.h"

// Sv32 PTE fields (priv spec 4.3.2).
enum {
  kPteV = 1 << 0,
  kPteR = 1 << 1,
  kPteW = 1 << 2,
  kPteX = 1 << 3,
  kPteU = 1 << 4,
  kPteA = 1 << 6,
  kPteD = 1 << 7,
};

// ---- PMP (priv spec 3.7) ----

static uint8_t PmpCfg(const Riscv32State* s, int i) {
  return (uint8_t)(s->pmpcfg[i / 4] >> (8 * (i % 4)));
}

// Decodes entry i's byte range; returns 0 for inactive (A=OFF) or empty
// ranges. NA4/NAPOT follow the raw pmpaddr encoding; TOR entries read
// pmpaddr[i-1] as the lower bound and mask both to the 16-byte grain (G=2).
// pmpaddr is 32 bits holding physical address bits [33:2] (priv spec 3.7.1),
// so a NAPOT with every bit set covers the whole 34-bit physical space.
static int PmpRegion(const Riscv32State* s, int i, uint64_t* base, uint64_t* size) {
  uint8_t cfg = PmpCfg(s, i);
  uint64_t a = cfg & kPmpAMask;
  uint64_t addr = s->pmpaddr[i];
  if (a == kPmpATor) {
    uint64_t lo = i > 0 ? (s->pmpaddr[i - 1] & ~3u) : 0;
    uint64_t hi = addr & ~3u;
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
    uint32_t inv = ~(uint32_t)addr;
    if (inv == 0) {  // all ones: the whole 34-bit address space
      *base = 0;
      *size = kSv32PhysSize;
      return 1;
    }
    int n = 0;  // count of trailing ones in pmpaddr
    while (!((inv >> n) & 1)) n++;
    *base = (addr >> n) << (n + 2);
    *size = 1ULL << (n + 2);
    return 1;
  }
  return 0;  // A = OFF
}

int Riscv32PmpAllowed(const Riscv32State* s, uint64_t addr, uint64_t len, int acc, int mode) {
  int need_r = (acc == acc_read || acc == acc_amo);
  int need_w = (acc == acc_write || acc == acc_amo);
  int need_x = (acc == acc_ifetch);
  int active = 0;
  for (int i = 0; i < kPmpCount; i++) {
    uint64_t base, size;
    if (!PmpRegion(s, i, &base, &size)) continue;
    active = 1;
    int any = addr < base + size && addr + len > base;
    if (!any) continue;
    // The lowest-numbered matching entry decides; an access that straddles
    // an entry boundary is denied (xiangshanNEMU pmp_check_permission).
    if (!(addr >= base && addr + len <= base + size)) return 0;
    uint8_t cfg = PmpCfg(s, i);
    if (mode == kPrivMachine && !(cfg & kPmpL)) return 1;  // M passes unlocked
    if ((need_r && !(cfg & kPmpR)) || (need_w && !(cfg & kPmpW)) || (need_x && !(cfg & kPmpX)))
      return 0;
    return 1;
  }
  if (!active) return 1;  // no entry ever configured: allowed (QEMU pmp.c
                         // pmp_hart_has_privs treats zero active rules as
                         // allow-all — bare-metal loaders jump to S-mode
                         // without any SBI to program PMP first. gem5 pmp.cc
                         // follows the priv spec 3.7 wording instead and
                         // fails S/U here; the spike machine models QEMU.)
  return mode == kPrivMachine;  // rules exist but none match: M passes, S/U are denied
}

// ---- Sv32 (priv spec 4.3.2 / 5.3) ----

static int AccessFaultCause(int acc) {
  if (acc == acc_ifetch) return kExFetchFault;
  if (acc == acc_read) return kExLoadFault;
  return kExStoreFault;
}

static int PageFaultCause(int acc) {
  if (acc == acc_ifetch) return kExFetchPageFault;
  if (acc == acc_read) return kExLoadPageFault;
  return kExStorePageFault;
}

// PTE fetch: a physical 4-byte read checked as S-mode (the reference walks
// page tables under MODE_S regardless of the trapped privilege).
static uint32_t PteRead(frame* f, Riscv32State* s, uint64_t pte_addr, int acc) {
  if (!Riscv32PmpAllowed(s, pte_addr, kSv32PteSize, acc_read, kPrivSupervisor) ||
      BusProbe(f->cpu->bus, pte_addr, kSv32PteSize, NULL) != 0) {
    raise_(f, AccessFaultCause(acc), pte_addr);
  }
  return (uint32_t)BusRead(f->cpu->bus, pte_addr, kSv32PteSize);
}

int Riscv32Translate(frame* f, Riscv32State* s, uint64_t vaddr, int acc, uint64_t* paddr) {
  vaddr &= kXlenMask;  // RV32: the virtual address is 32 bits wide
  // Effective privilege for data accesses under MPRV uses MPP (priv spec
  // 3.1.6.6); instruction fetch always uses the current mode.
  int eff = s->priv;
  if ((s->mstatus & kMstatusMprv) && acc != acc_ifetch)
    eff = (int)((s->mstatus & kMstatusMppMask) >> 11);
  uint64_t satp_mode = (s->satp >> 31) & 1;
  if (eff == kPrivMachine || satp_mode == kSatpModeBare) {
    *paddr = vaddr;
    return 0;
  }

  // Sv32 has no canonical-address check: every 32-bit VA is valid.
  uint64_t ppn_base = s->satp & kSatpPpnMask;
  uint32_t pte = 0;
  uint64_t pte_addr = 0;
  int level;
  for (level = kSv32Levels - 1; level >= 0; level--) {
    uint64_t vpn = (vaddr >> (kSv32PageShift + kSv32VpnBits * level)) & kSv32VpnMask;
    pte_addr = (ppn_base << kSv32PageShift) + vpn * kSv32PteSize;
    pte = PteRead(f, s, pte_addr, acc);
    if (!(pte & kPteV) || (!(pte & kPteR) && (pte & kPteW))) {
      raise_(f, PageFaultCause(acc), vaddr);
    }
    if (pte & (kPteR | kPteX)) break;  // leaf
    // Non-leaf: a pointer to the next level. Its A/D/U/G bits carry no
    // walk meaning — the priv spec sentence binds the page-table writer,
    // not the walker: QEMU's ptw ignores them, and real OSes do set them
    // (HelenOS 0.14.1 writes V|G|A|D pointers).
    ppn_base = (pte >> 10) & kSatpPpnMask;
  }

  // Permission check (xiangshanNEMU check_permission): U pages from S need
  // SUM (never for fetch); U mode needs the U bit; MXR turns X-only pages
  // readable.
  int ok = 1;
  if (eff == kPrivUser && !(pte & kPteU)) ok = 0;
  if ((pte & kPteU) && eff == kPrivSupervisor && (!(s->mstatus & kMstatusSum) || acc == acc_ifetch))
    ok = 0;
  if (acc == acc_ifetch) {
    if (!ok || !(pte & kPteX)) goto page_fault;
  } else if (acc == acc_read) {
    if (!ok || (!((pte & kPteR) || ((s->mstatus & kMstatusMxr) && (pte & kPteX))))) goto page_fault;
  } else {  // write and AMO need W (the AMO read side faults as a store)
    if (!ok || !(pte & kPteW)) goto page_fault;
  }
  // A/D handling (priv spec 4.3.1 + Svadu): the A bit is updated by
  // hardware unconditionally on any successful access; the D bit matters
  // for stores and AMOs only (a fetch never sets D) and its update is
  // Svadu's ADUE-gated choice — set silently when menvcfg.ADUE=1, otherwise
  // a page fault hands the job to software. riscv-tests rv32si-p-dirty pins
  // exactly this split.
  uint32_t new_pte = pte;
  int is_store = acc == acc_write || acc == acc_amo;
  if (is_store && !(pte & kPteD)) {
    if (!(s->menvcfg & kMenvcfgAdue)) goto page_fault;
    new_pte |= kPteD;
  }
  if (!(pte & kPteA)) new_pte |= kPteA;
  if (new_pte != pte) {
    // The PTE lives in RAM on every machine that pages; a missing region
    // means it no longer does, which is an access fault.
    if (BusProbe(f->cpu->bus, pte_addr, kSv32PteSize, NULL) != 0) {
      raise_(f, AccessFaultCause(acc), vaddr);
    }
    BusWrite(f->cpu->bus, pte_addr, kSv32PteSize, new_pte);
    pte = new_pte;
  }

  uint64_t ppn = (pte >> 10) & kSatpPpnMask;
  if (level > 0) {
    // Superpage: PPN[0] must be zero for a 4 MiB page (priv spec 4.3.2),
    // which is also the encode rule the reference enforces.
    uint64_t low = kSv32VpnMask;
    if (ppn & low) goto page_fault;
    uint64_t off_mask = kSv32SuperpageMask;
    *paddr = ((ppn & ~low) << kSv32PageShift) | (vaddr & off_mask);
  } else {
    *paddr = (ppn << kSv32PageShift) | (vaddr & kSv32OffsetMask);
  }
  return 0;

page_fault:
  raise_(f, PageFaultCause(acc), vaddr);
  return -1;  // not reached
}
