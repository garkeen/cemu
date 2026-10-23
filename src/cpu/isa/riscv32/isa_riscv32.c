// The riscv32 ops table: the machine layer's handle on the interpreter
// (init, step, irq, dumps) plus the debug hooks the hub queries. The
// interpreter itself is exec.c; the semantic layers are csr.c (CSRs,
// traps), mmu.c (Sv32/PMP), fp.c (IEEE-754).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/isa/isa.h"
#include "debug/debug.h"
#include "host/host.h"
#include "riscv32.h"

void riscv32_init(CpuState* cpu) {
  Riscv32State* s = (Riscv32State*)cpu->priv;
  if (!s) {
    s = (Riscv32State*)calloc(1, sizeof(Riscv32State));
    cpu->priv = s;
  } else {
    memset(s, 0, sizeof(*s));
  }
  s->priv = kPrivMachine;
  s->mstatus = 0;                    // RV32: no UXL/SXL fields
  s->misa = kMisaSupported;          // RV32IMAFDC + S/U
  s->tdata1[0] = kTrigTypeMcontrol6;  // both triggers are mcontrol6 (Sdtrig)
  s->tdata1[1] = kTrigTypeMcontrol6;
  s->stimecmp = ~0ULL;               // Sstc: no S-level timer interrupt until programmed
  // How boards drive our interrupt lines: line is a mip bit (platform.h).
  // Boards call this hook instead of naming a CPU-model function.
  cpu->set_irq = riscv32_set_ext_irq;
}

void riscv32_dump_regs(const CpuState* cpu) {
  const Riscv32State* s = (const Riscv32State*)cpu->priv;
  char buf[160];
  static const char* names[32] = {"zero", "ra", "sp",  "gp",  "tp", "t0", "t1", "t2",
                                  "s0",   "s1", "a0",  "a1",  "a2", "a3", "a4", "a5",
                                  "a6",   "a7", "s2",  "s3",  "s4", "s5", "s6", "s7",
                                  "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
  for (int i = 0; i < 32; i += 4) {
    int n = snprintf(buf, sizeof(buf), "%4s=%08llx %4s=%08llx %4s=%08llx %4s=%08llx\n", names[i],
                     (unsigned long long)(uint32_t)cpu->gpr[i], names[i + 1],
                     (unsigned long long)(uint32_t)cpu->gpr[i + 1], names[i + 2],
                     (unsigned long long)(uint32_t)cpu->gpr[i + 2], names[i + 3],
                     (unsigned long long)(uint32_t)cpu->gpr[i + 3]);
    HostWriteErr(buf, (size_t)n);
  }
  int n = snprintf(buf, sizeof(buf), "  pc=%08llx mstatus=%08llx mcause=%08llx mtval=%08llx\n",
                   (unsigned long long)(uint32_t)cpu->pc, (unsigned long long)(uint32_t)s->mstatus,
                   (unsigned long long)(uint32_t)s->mcause, (unsigned long long)(uint32_t)s->mtval);
  HostWriteErr(buf, (size_t)n);
}

// ---- debug hooks (AGENTS.md §X) ----------------------------------------------

// Cause names for the trap table row (priv spec 1.3 table 1.2 plus the
// interrupt bit forms).
static const char* riscv32_cause_name(uint64_t cause) {
  if (cause & kIntBit) return "intr";
  switch (cause) {
    case kExFetchMisaligned:
      return "fetch-misalign";
    case kExFetchFault:
      return "fetch-fault";
    case kExIllegal:
      return "illegal";
    case kExBreakpoint:
      return "breakpoint";
    case kExLoadMisaligned:
      return "load-misalign";
    case kExLoadFault:
      return "load-fault";
    case kExStoreMisaligned:
      return "store-misalign";
    case kExStoreFault:
      return "store-fault";
    case kExUserEcall:
      return "u-ecall";
    case kExSupervisorEcall:
      return "s-ecall";
    case kExMachineEcall:
      return "m-ecall";
    case kExFetchPageFault:
      return "fetch-page";
    case kExLoadPageFault:
      return "load-page";
    case kExStorePageFault:
      return "store-page";
    default:
      return NULL;
  }
}

static int riscv32_has_fpr(frame* f) {
  return ((const Riscv32State*)f->priv)->mstatus & kMstatusFs ? 1 : 0;
}

static void riscv32_debug_state_line(char* buf, int cap, frame* f) {
  Riscv32State* s = f->priv;
  snprintf(buf, (size_t)cap, "x: priv=%d mstatus=%08llx mie=%08llx mip=%08llx wait=%d", s->priv,
           (unsigned long long)(uint32_t)s->mstatus, (unsigned long long)(uint32_t)s->mie,
           (unsigned long long)(uint32_t)s->mip, f->cpu->wait);
}

// ---- gdb stub (stage 3.5) -----------------------------------------------------

static void Le32Put(uint8_t** p, uint32_t v) {
  for (int i = 0; i < 4; i++) (*p)[i] = (uint8_t)(v >> (8 * i));
  *p += 4;
}

static uint32_t Le32Get(const uint8_t** p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= (uint32_t)(*p)[i] << (8 * i);
  *p += 4;
  return v;
}

// gdb's riscv32 integer register file (org.gnu.gdb.riscv.cpu): x0..x31 then
// pc, 4 bytes each (gdb's riscv:rv32 target description).
static int riscv32_gdb_read_regs(CpuState* cpu, uint8_t* buf, int cap) {
  if (cap < 33 * 4) return 0;
  uint8_t* p = buf;
  for (int i = 0; i < 32; i++) Le32Put(&p, (uint32_t)cpu->gpr[i]);
  Le32Put(&p, (uint32_t)cpu->pc);
  return 33 * 4;
}

static int riscv32_gdb_write_regs(CpuState* cpu, const uint8_t* buf, int len) {
  if (len < 33 * 4) return -1;
  const uint8_t* p = buf;
  for (int i = 0; i < 32; i++) {
    uint32_t v = Le32Get(&p);
    if (i) cpu->gpr[i] = v;  // x0 stays hardwired to zero
  }
  cpu->pc = Le32Get(&p);
  return 0;
}

static int riscv32_gdb_last_trap(CpuState* cpu, uint64_t* seq) {
  Riscv32State* s = (Riscv32State*)cpu->priv;
  *seq = s->trap_seq;
  return s->trap_signal;
}

const isa_ops k_isa_riscv32 = {
    "riscv32",
    243,  // EM_RISCV (shared with riscv64; elf_class below disambiguates)
    riscv32_init,
    riscv32_step,
    riscv32_dump_regs,
    // debug hooks
    riscv32_cause_name,
    NULL,  // no flag word
    NULL,  // (flag_word)
    riscv32_has_fpr,
    riscv32_debug_state_line,
    // gdb stub hooks
    riscv32_gdb_read_regs,
    riscv32_gdb_write_regs,
    riscv32_gdb_last_trap,
    NULL,  // gdb_pc: the committed pc is already linear here (cpu/step.h)
    NULL,  // gdb_set_watch / gdb_clear_watch / gdb_watch_hits: no RSP channel
           // onto the trigger module yet (AGENTS.md 简化登记 D6)
    NULL,
    NULL,
    1,  // ELFCLASS32
};
