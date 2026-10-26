// Sv39/Sv48 address translation and PMP enforcement. Semantics translated from
// xiangshanNEMU src/isa/riscv64/system/mmu.c (ptw, check_permission,
// pmp_check_permission_with_mode); divergences follow the priv spec or are
// calibrated against QEMU behavior where noted. Successful translations are
// cached (RiscvTlbEntry): page walk, PMP and A/D handling all happen at fill
// time, and the cache stores the host backing of plain RAM pages so the hot
// load/store path is a direct memcpy. riscv_step invalidates the cache on any
// context change; sfence.vma invalidates it directly.
//
// Faults raise through the frame (raise_): translation failures page-fault,
// PMP and unmapped-physical failures access-fault, each with the faulting
// virtual address in tval.
#include "debug/debug.h"
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

static uint8_t PmpCfg(const RiscvState* s, int i) {
  return (uint8_t)(s->pmpcfg[i / 8] >> (8 * (i % 8)));
}

// Decodes entry i's byte range; returns 0 for inactive (A=OFF) or empty
// ranges. NA4/NAPOT follow the raw pmpaddr encoding (QEMU
// target/riscv/pmp.c pmp_napot_get_range); TOR entries read pmpaddr[i-1]
// as the lower bound and mask both to the 16-byte grain (G=2).
static int PmpRegion(const RiscvState* s, int i, uint64_t* base, uint64_t* size) {
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
    // A run of n trailing ones encodes a 2^(n+3)-byte region (priv spec
    // 3.7.1; OpenSBI sbi_pmp_decode's len = ctz(~addr) + PMP_SHIFT + 1).
    int n = 0;  // count of trailing ones in pmpaddr
    while (!(inv >> n & 1)) n++;
    *base = (addr >> n) << (n + 2);
    *size = 1ULL << (n + 3);
    return 1;
  }
  return 0;  // A = OFF
}

int RiscvPmpAllowed(const RiscvState* s, uint64_t addr, uint64_t len, int acc, int mode) {
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
    if (!(addr >= base && addr + len <= base + size)) {
      DebugPmpDeny(addr, (int)len, acc, mode, i, PmpCfg(s, i), base, size);
      return 0;
    }
    uint8_t cfg = PmpCfg(s, i);
    if (mode == kPrivMachine && !(cfg & kPmpL)) return 1;  // M passes unlocked
    if ((need_r && !(cfg & kPmpR)) || (need_w && !(cfg & kPmpW)) || (need_x && !(cfg & kPmpX))) {
      DebugPmpDeny(addr, (int)len, acc, mode, i, cfg, base, size);
      return 0;
    }
    return 1;
  }
  if (!active) return 1;  // no entry ever configured: allowed (QEMU pmp.c
                         // pmp_hart_has_privs treats zero active rules as
                         // allow-all — bare-metal loaders jump to S-mode
                         // without any SBI to program PMP first. gem5 pmp.cc
                         // follows the priv spec 3.7 wording instead and
                         // fails S/U here; the spike machine models QEMU.)
  if (mode == kPrivMachine) return 1;  // rules exist but none match: M passes
  DebugPmpDeny(addr, (int)len, acc, mode, -1, 0, 0, 0);
  return 0;  // S/U denied when no rule matched
}

// ---- Sv39/Sv48 (priv spec 4.3.2 / 4.4.1 / 5.3) ----

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

// PTE fetch: a physical 8-byte read checked as S-mode (xiangshanNEMU mmu.c
// walks page tables under MODE_S regardless of the trapped privilege).
static uint64_t PteRead(frame* f, RiscvState* s, uint64_t pte_addr, int acc) {
  if (!RiscvPmpAllowed(s, pte_addr, 8, acc_read, kPrivSupervisor) ||
      BusProbe(f->cpu->bus, pte_addr, 8, NULL) != 0) {
    raise_paddr_(f, AccessFaultCause(acc), pte_addr, pte_addr);
  }
  return BusRead(f->cpu->bus, pte_addr, 8);
}

int RiscvTranslate(frame* f, RiscvState* s, uint64_t vaddr, int size, int acc, uint64_t* paddr,
                   uint8_t** host) {
  if (host) *host = NULL;
  // Effective privilege for data accesses under MPRV uses MPP (priv spec
  // 3.1.6.6); instruction fetch always uses the current mode.
  int eff = s->priv;
  if ((s->mstatus & kMstatusMprv) && acc != acc_ifetch)
    eff = (int)((s->mstatus & kMstatusMppMask) >> 11);
  uint64_t satp_mode = (s->satp >> 60) & 0xf;
  if (eff == kPrivMachine || satp_mode == kSatpModeBare) {
    *paddr = vaddr;
    if (!RiscvPmpAllowed(s, vaddr, (uint64_t)size, acc, s->priv))
      raise_paddr_(f, AccessFaultCause(acc), vaddr, vaddr);
    if (host) BusRamPage(f->cpu->bus, vaddr & ~0xfffULL, host);
    return 0;
  }

  // Translation cache: a hit skips the walk, its PMP checks and the A/D
  // writes. Each entry records the context its decision was made under, so a
  // miss on any field simply refills; sfence.vma bumps the generation. Set
  // index mixes the access class in because permissions are per access type.
  uint64_t vpn = vaddr >> 12;
  uint64_t msb = s->mstatus & (kMstatusSum | kMstatusMxr);
  uint64_t menv = s->menvcfg & kMenvcfgAdue;
  uint32_t set = (uint32_t)(vpn ^ ((uint64_t)acc * 0x9E3779B97F4A7C15ULL)) & (kTlbSets - 1);
  RiscvTlbEntry* ways = &s->tlb[(size_t)set * kTlbWays];
  for (int w = 0; w < kTlbWays; w++) {
    RiscvTlbEntry* e = &ways[w];
    if (e->gen == s->tlb_gen && e->vpn == vpn && e->acc == (uint8_t)acc &&
        e->satp == s->satp && e->priv == s->priv && e->eff == (uint8_t)eff &&
        e->mstatus == msb && e->menvcfg == menv && e->pmp_gen == s->pmp_gen) {
      *paddr = e->pbase | (vaddr & 0xfff);
      if (host) *host = e->host;
      return 0;
    }
  }

  // Sv39 walks three levels, Sv48 four (priv spec 4.4.1); the walk itself is
  // identical, only the top level and the canonical-address window differ.
  int top = satp_mode == kSatpModeSv48 ? 3 : 2;
  // Canonical-address check (priv spec 5.3): the bits above the mode's VA
  // width must all copy the top VA bit — bits 63:39 copy bit 38 for Sv39,
  // bits 63:48 copy bit 47 for Sv48.
  if (top == 3) {
    int64_t v48 = (int64_t)(vaddr << 16) >> 16;
    if ((uint64_t)v48 != vaddr) raise_(f, PageFaultCause(acc), vaddr);
  } else {
    int64_t v39 = (int64_t)(vaddr << 25) >> 25;
    if ((uint64_t)v39 != vaddr) raise_(f, PageFaultCause(acc), vaddr);
  }

  uint64_t ppn_base = s->satp & ((1ULL << 44) - 1);
  uint64_t pte = 0;
  uint64_t pte_addr = 0;
  int level;
  for (level = top; level >= 0; level--) {
    uint64_t vpn = (vaddr >> (12 + 9 * level)) & 0x1ff;
    pte_addr = (ppn_base << 12) + vpn * 8;
    pte = PteRead(f, s, pte_addr, acc);
    if (!(pte & kPteV) || (!(pte & kPteR) && (pte & kPteW)) || (pte & kPteReserved)) {
      raise_(f, PageFaultCause(acc), vaddr);
    }
    if (pte & (kPteR | kPteX)) break;  // leaf
    // Non-leaf: a pointer to the next level. Its A/D/U/G bits carry no
    // walk meaning — the priv spec 4.3.2 sentence ("the D, A, and U bits
    // must all be cleared") binds the page-table writer, not the walker:
    // QEMU's ptw ignores them, and real OSes do set them (HelenOS 0.14.1
    // writes V|G|A|D pointers). xiangshanNEMU faults here, but its guests
    // (OpenSBI/Linux) never set the bits, so its check is uncalibrated.
    ppn_base = pte >> 10;
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
  } else {  // write and AMO need W (the AMO read side faults as a store,
            // xiangshanNEMU uses EX_SPF for cpu.amo)
    if (!ok || !(pte & kPteW)) goto page_fault;
  }
  // A/D handling (priv spec 4.3.1 + Svadu): the A bit is updated by
  // hardware unconditionally on any successful access; the D bit matters
  // for stores and AMOs only (a fetch never sets D) and its update is
  // Svadu's ADUE-gated choice — set silently when menvcfg.ADUE=1, otherwise
  // a page fault hands the job to software. riscv-tests rv64si-p-dirty pins
  // exactly this split: a store to D=0 faults to the handler (which sets D
  // by hand), while accesses that only need A proceed silently.
  uint64_t new_pte = pte;
  int is_store = acc == acc_write || acc == acc_amo;
  if (is_store && !(pte & kPteD)) {
    if (!(s->menvcfg & kMenvcfgAdue)) goto page_fault;
    new_pte |= kPteD;
  }
  if (!(pte & kPteA)) new_pte |= kPteA;
  if (new_pte != pte) {
    // The PTE lives in RAM on every machine that pages; a missing region
    // means it no longer does, which is an access fault.
    if (BusProbe(f->cpu->bus, pte_addr, 8, NULL) != 0) {
      raise_paddr_(f, AccessFaultCause(acc), vaddr, pte_addr);
    }
    BusWrite(f->cpu->bus, pte_addr, 8, new_pte);
    pte = new_pte;
  }

  uint64_t ppn = pte >> 10;
  if (level > 0) {
    // Superpage: PPN bits below the leaf level must be zero (priv spec
    // 4.3.2), which also keeps PPN[2] within 27 bits for a Sv39 leaf.
    uint64_t low = (1ULL << (9 * level)) - 1;
    if (ppn & low) goto page_fault;
    uint64_t off_mask = (1ULL << (12 + 9 * level)) - 1;
    *paddr = ((ppn & ~low) << 12) | (vaddr & off_mask);
  } else {
    *paddr = (ppn << 12) | (vaddr & 0xfff);
  }
  // PMP for the final physical access is decided once, at fill time: a cache
  // hit may then skip the 16-entry scan. The entry records pmp_gen, so a PMP
  // CSR write makes every old decision miss.
  if (!RiscvPmpAllowed(s, *paddr, (uint64_t)size, acc, s->priv))
    raise_paddr_(f, AccessFaultCause(acc), vaddr, *paddr);
  uint8_t* h = NULL;
  BusRamPage(f->cpu->bus, *paddr & ~0xfffULL, &h);
  // Prefer a slot invalidated by sfence; otherwise alternate ways so one hot
  // page cannot pin a set forever.
  int w = ways[0].gen != s->tlb_gen ? 0 : ways[1].gen != s->tlb_gen ? 1 : (int)(s->tlb_clock++ & 1);
  RiscvTlbEntry* te = &ways[w];
  te->vpn = vpn;
  te->pbase = *paddr & ~0xfffULL;
  te->host = h;
  te->acc = (uint8_t)acc;
  te->gen = s->tlb_gen;
  te->satp = s->satp;
  te->priv = s->priv;
  te->eff = (uint8_t)eff;
  te->mstatus = msb;
  te->menvcfg = menv;
  te->pmp_gen = s->pmp_gen;
  if (host) *host = h;
  return 0;

page_fault:
  raise_(f, PageFaultCause(acc), vaddr);
  return -1;  // not reached
}
