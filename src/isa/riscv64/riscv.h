#ifndef CEMU_ISA_RISCV64_RISCV_H
#define CEMU_ISA_RISCV64_RISCV_H

#include "core/cpu.h"
#include "core/bus.h"

// Operation selectors for the fp.c entry points.
enum {
  kFpAdd = 0,
  kFpSub,
  kFpMul,
  kFpDiv,
  kFpEq,
  kFpLt,
  kFpLe,
};

// Which counter CSR the current instruction wrote; Step suppresses that
// counter's own increment for the writing instruction (priv spec Zicntr: a
// counter write overrides the implicit increment; rv64mi-p-instret_overflow
// anchors the minstret case).
enum {
  kCounterNone = 0,
  kCounterMcycle = 1,
  kCounterMinstret = 2,
};

// Bit fields from the privileged spec, named once here. 64-bit constants
// need static const: C enum constants must fit int.
static const uint64_t kMstatusMie = 1ULL << 3;
static const uint64_t kMstatusMpie = 1ULL << 7;
static const uint64_t kMstatusMppMask = 3ULL << 11;
static const uint64_t kMstatusFs = 3ULL << 13;
static const uint64_t kMstatusUxlMask = 3ULL << 32;
static const uint64_t kMstatusUxlValue = 2ULL << 32;  // UXL = 2: RV64 U-mode
static const uint64_t kMcountinhibitCy = 1ULL << 0;
static const uint64_t kMcountinhibitIr = 1ULL << 2;
static const uint64_t kMisaMxl64 = 2ULL << 62;  // MXL = 2: RV64
static const uint64_t kMisaA = 1ULL << 0;
static const uint64_t kMisaC = 1ULL << 2;
static const uint64_t kMisaD = 1ULL << 3;
static const uint64_t kMisaF = 1ULL << 4;
static const uint64_t kMisaI = 1ULL << 8;
static const uint64_t kMisaM = 1ULL << 12;
static const uint64_t kMisaSupported =
    kMisaMxl64 | kMisaA | kMisaC | kMisaD | kMisaF | kMisaI | kMisaM;
// tcontrol MTE/MPTE (RISC-V debug spec, tcontrol register)
static const uint64_t kTcontrolMte = 1ULL << 7;
static const uint64_t kTcontrolMpte = 1ULL << 6;

typedef struct RiscvState {
  uint64_t gpr[32];
  uint64_t fpr[32];
  uint64_t misa;
  uint64_t mstatus;
  uint64_t mie;
  uint64_t mip;
  uint64_t mtvec;
  uint64_t mscratch;
  uint64_t mepc;
  uint64_t mcause;
  uint64_t mtval;
  uint64_t medeleg;
  uint64_t mideleg;
  uint64_t mcounteren;
  uint64_t mcycle;
  uint64_t minstret;
  uint64_t mcountinhibit;
  uint64_t stvec;
  uint64_t sscratch;
  uint64_t sepc;
  uint64_t scause;
  uint64_t stval;
  uint64_t satp;
  uint64_t tselect;
  uint64_t tcontrol;
  uint64_t pmpcfg[16];
  uint64_t pmpaddr[64];
  uint8_t priv;
  uint8_t frm;
  uint8_t fflags;
  uint8_t counter_written;
  int res_valid;
  uint64_t res_addr;
  struct {
    uint64_t pc;
    uint32_t inst;
    uint8_t len;
  } trace[64];
  int trace_count;
  int trace_pos;
} RiscvState;

enum {
  kExFetchMisaligned = 0,
  kExFetchFault = 1,
  kExIllegal = 2,
  kExBreakpoint = 3,
  kExLoadMisaligned = 4,
  kExLoadFault = 5,
  kExStoreMisaligned = 6,
  kExStoreFault = 7,
  kExUserEcall = 8,
  kExMachineEcall = 11,
};

enum {
  kFflagNX = 1 << 0,
  kFflagUF = 1 << 1,
  kFflagOF = 1 << 2,
  kFflagDZ = 1 << 3,
  kFflagNV = 1 << 4,
};

enum {
  kRmRne = 0,
  kRmRtz = 1,
  kRmRdn = 2,
  kRmRup = 3,
  kRmRmm = 4,
  kRmDyn = 7,
};

void RiscvInit(CpuState *cpu);
void RiscvStep(CpuState *cpu);
void RiscvDumpRegs(const CpuState *cpu);

// csr.c
int RiscvCsrRead(RiscvState *s, uint64_t addr, uint64_t *out);
// next_pc is the address of the instruction that follows the CSR write; the
// misa WARL check needs it to reject C-clear that would misalign the next
// fetch (IALIGN=32).
int RiscvCsrWrite(RiscvState *s, uint64_t addr, uint64_t val, uint64_t next_pc);
void RiscvTrap(CpuState *cpu, RiscvState *s, uint64_t cause, uint64_t tval);
void RiscvDeliverPendingInterrupt(CpuState *cpu, RiscvState *s);
uint64_t RiscvMret(RiscvState *s);

// fp.c
uint64_t RiscvFpBinaryS(RiscvState *s, uint64_t ra, uint64_t rb, int kind,
                        int rm);
uint64_t RiscvFpBinaryD(RiscvState *s, uint64_t ra, uint64_t rb, int kind,
                        int rm);
uint64_t RiscvFpSqrtS(RiscvState *s, uint64_t ra, int rm);
uint64_t RiscvFpSqrtD(RiscvState *s, uint64_t ra, int rm);
uint64_t RiscvFpFmaS(RiscvState *s, uint64_t ra, uint64_t rb, uint64_t rc,
                     int rm);
uint64_t RiscvFpFmaD(RiscvState *s, uint64_t ra, uint64_t rb, uint64_t rc,
                     int rm);
uint64_t RiscvFpMinMaxS(RiscvState *s, uint64_t ra, uint64_t rb, int is_max);
uint64_t RiscvFpMinMaxD(RiscvState *s, uint64_t ra, uint64_t rb, int is_max);
uint64_t RiscvFpCmpS(RiscvState *s, uint64_t ra, uint64_t rb, int kind);
uint64_t RiscvFpCmpD(RiscvState *s, uint64_t ra, uint64_t rb, int kind);
uint64_t RiscvFpClassS(uint64_t ra);
uint64_t RiscvFpClassD(uint64_t ra);
uint64_t RiscvFpSgnjS(uint64_t ra, uint64_t rb, int kind);
uint64_t RiscvFpSgnjD(uint64_t ra, uint64_t rb, int kind);
uint64_t RiscvFpF2IS(RiscvState *s, uint64_t ra, int to_signed, int dbits,
                     int rm);
uint64_t RiscvFpF2ID(RiscvState *s, uint64_t ra, int to_signed, int dbits,
                     int rm);
uint64_t RiscvFpI2FS(RiscvState *s, uint64_t val, int val_signed, int bits,
                     int rm);
uint64_t RiscvFpI2FD(RiscvState *s, uint64_t val, int val_signed, int bits,
                     int rm);
uint64_t RiscvFpCvtSD(RiscvState *s, uint64_t ra, int rm);  // double -> single
uint64_t RiscvFpCvtDS(RiscvState *s, uint64_t ra, int rm);  // single -> double

void RiscvTracePush(RiscvState *s, uint64_t pc, uint32_t inst, int len);
void RiscvTraceDump(const RiscvState *s);

#endif
