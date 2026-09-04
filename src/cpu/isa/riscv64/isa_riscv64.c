// The riscv64 ops table: the machine layer's handle on the interpreter
// (init, step, irq, dumps) plus the debug hooks the hub queries. The
// interpreter itself is exec.c; the semantic layers are csr.c (CSRs,
// traps), mmu.c (Sv39/PMP), fp.c (IEEE-754).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "riscv.h"
#include "cpu/isa/isa.h"
#include "debug/debug.h"
#include "host/host.h"

void riscv_init(CpuState *cpu) {
  RiscvState *s = (RiscvState *)cpu->priv;
  if (!s) {
    s = (RiscvState *)calloc(1, sizeof(RiscvState));
    cpu->priv = s;
  } else {
    memset(s, 0, sizeof(*s));
  }
  s->priv = kPrivMachine;
  s->mstatus = 2ULL << 32;  // mstatus.UXL = 2 (U-mode is RV64)
  s->misa = kMisaSupported;  // RV64IMAFDC + S-mode
  s->tdata[0] = kTrigTypeMcontrol6;  // both triggers are mcontrol6 (Sdtrig)
  s->stimecmp = ~0ULL;  // Sstc: no S-level timer interrupt until programmed
  // How boards drive our interrupt lines: line is a mip bit (platform.h).
  // Boards call this hook instead of naming a CPU-model function.
  cpu->set_irq = riscv_set_ext_irq;
}

void riscv_dump_regs(const CpuState *cpu) {
  const RiscvState *s = (const RiscvState *)cpu->priv;
  char buf[160];
  static const char *names[32] = {
      "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1",
      "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3",
      "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4",
      "t5", "t6"};
  for (int i = 0; i < 32; i += 4) {
    int n = snprintf(buf, sizeof(buf),
                     "%4s=%016llx %4s=%016llx %4s=%016llx %4s=%016llx\n",
                     names[i], (unsigned long long)cpu->gpr[i],
                     names[i + 1], (unsigned long long)cpu->gpr[i + 1],
                     names[i + 2], (unsigned long long)cpu->gpr[i + 2],
                     names[i + 3], (unsigned long long)cpu->gpr[i + 3]);
    HostWriteErr(buf, (size_t)n);
  }
  int n = snprintf(buf, sizeof(buf),
                   "  pc=%016llx mstatus=%016llx mcause=%016llx mtval=%016llx\n",
                   (unsigned long long)cpu->pc,
                   (unsigned long long)s->mstatus,
                   (unsigned long long)s->mcause,
                   (unsigned long long)s->mtval);
  HostWriteErr(buf, (size_t)n);
}

// ---- debug hooks (AGENTS.md §X) ----------------------------------------------

// Cause names for the trap table row (priv spec 1.3 table 1.2 plus the
// interrupt bit forms).
static const char *riscv_cause_name(uint64_t cause) {
  if (cause & kIntBit) return "intr";
  switch (cause) {
    case kExFetchMisaligned: return "fetch-misalign";
    case kExFetchFault: return "fetch-fault";
    case kExIllegal: return "illegal";
    case kExBreakpoint: return "breakpoint";
    case kExLoadMisaligned: return "load-misalign";
    case kExLoadFault: return "load-fault";
    case kExStoreMisaligned: return "store-misalign";
    case kExStoreFault: return "store-fault";
    case kExUserEcall: return "u-ecall";
    case kExSupervisorEcall: return "s-ecall";
    case kExMachineEcall: return "m-ecall";
    case kExFetchPageFault: return "fetch-page";
    case kExLoadPageFault: return "load-page";
    case kExStorePageFault: return "store-page";
    default: return NULL;
  }
}

static int riscv_has_fpr(frame *f) {
  return ((const RiscvState *)f->priv)->mstatus & kMstatusFs ? 1 : 0;
}

static void riscv_debug_state_line(char *buf, int cap, frame *f) {
  RiscvState *s = f->priv;
  snprintf(buf, (size_t)cap,
           "x: priv=%d mstatus=%016llx mie=%016llx mip=%016llx wait=%d",
           s->priv, (unsigned long long)s->mstatus,
           (unsigned long long)s->mie, (unsigned long long)s->mip,
           f->cpu->wait);
}

const isa_ops k_isa_riscv64 = {
    "riscv64",
    243,  // EM_RISCV
    riscv_init,
    riscv_step,
    riscv_dump_regs,
    // debug hooks
    riscv_cause_name,
    NULL,  // no flag word
    NULL,  // (flag_word)
    riscv_has_fpr,
    riscv_debug_state_line,
};
