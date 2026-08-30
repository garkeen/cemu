#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "riscv.h"
#include "isa/isa.h"
#include "host/host.h"

enum { kPrivMachine = 3 };

void RiscvInit(CpuState *cpu) {
  RiscvState *s = (RiscvState *)cpu->priv;
  if (!s) {
    s = (RiscvState *)calloc(1, sizeof(RiscvState));
    cpu->priv = s;
  } else {
    memset(s, 0, sizeof(*s));
  }
  s->priv = kPrivMachine;
  s->mstatus = 2ULL << 32;  // mstatus.UXL = 2 (U-mode is RV64)
  s->misa = (2ULL << 62) | (1ULL << 0) | (1ULL << 2) | (1ULL << 3) |
            (1ULL << 4) | (1ULL << 8) | (1ULL << 12);  // RV64IMAFDC
}

void RiscvDumpRegs(const CpuState *cpu) {
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
                     names[i], (unsigned long long)s->gpr[i],
                     names[i + 1], (unsigned long long)s->gpr[i + 1],
                     names[i + 2], (unsigned long long)s->gpr[i + 2],
                     names[i + 3], (unsigned long long)s->gpr[i + 3]);
    HostWriteErr(buf, (size_t)n);
  }
  int n = snprintf(buf, sizeof(buf),
                   "  pc=%016llx mstatus=%016llx mcause=%016llx mtval=%016llx\n",
                   (unsigned long long)cpu->pc,
                   (unsigned long long)s->mstatus,
                   (unsigned long long)s->mcause,
                   (unsigned long long)s->mtval);
  HostWriteErr(buf, (size_t)n);
  n = snprintf(buf, sizeof(buf),
               " fa0=%016llx fa1=%016llx fa2=%016llx fa3=%016llx\n",
               (unsigned long long)s->fpr[10], (unsigned long long)s->fpr[11],
               (unsigned long long)s->fpr[12], (unsigned long long)s->fpr[13]);
  HostWriteErr(buf, (size_t)n);
  RiscvTraceDump(s);
}

const IsaOps kIsaRiscv64 = {
    "riscv64",
    243,  // EM_RISCV
    RiscvInit,
    RiscvStep,
    RiscvDumpRegs,
};
