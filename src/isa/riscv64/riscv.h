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

// Privilege modes (priv spec 3.1.1).
enum { kPrivUser = 0, kPrivSupervisor = 1, kPrivHypervisor = 2, kPrivMachine = 3 };

// mstatus fields (priv spec 3.1.6). 64-bit constants need static const:
// C enum constants must fit int.
static const uint64_t kMstatusSie = 1ULL << 1;
static const uint64_t kMstatusMie = 1ULL << 3;
static const uint64_t kMstatusSpie = 1ULL << 5;
static const uint64_t kMstatusMpie = 1ULL << 7;
static const uint64_t kMstatusSpp = 1ULL << 8;
static const uint64_t kMstatusMppMask = 3ULL << 11;
static const uint64_t kMstatusFs = 3ULL << 13;
static const uint64_t kMstatusMprv = 1ULL << 17;
static const uint64_t kMstatusSum = 1ULL << 18;
static const uint64_t kMstatusMxr = 1ULL << 19;
static const uint64_t kMstatusTvm = 1ULL << 20;
static const uint64_t kMstatusTw = 1ULL << 21;
static const uint64_t kMstatusTsr = 1ULL << 22;
static const uint64_t kMstatusUxlMask = 3ULL << 32;
static const uint64_t kMstatusUxlValue = 2ULL << 32;  // UXL = 2: RV64 U-mode
// Writable mstatus bits (xiangshanNEMU priv.c MSTATUS_WMASK_BASE 0x7e19aa |
// FS, plus MPRV/SUM/MXR/TVM/TW/TSR). UXL/MXL are WARL, not writable.
static const uint64_t kMstatusWmask =
    kMstatusSie | kMstatusMie | kMstatusSpie | kMstatusMpie | kMstatusSpp |
    kMstatusMppMask | kMstatusFs | kMstatusMprv | kMstatusSum | kMstatusMxr |
    kMstatusTvm | kMstatusTw | kMstatusTsr;
// sstatus view over mstatus (priv spec 3.1.6.3): readable = S-level fields +
// UXL + SD; XS is not implemented (no extension state) and reads 0.
static const uint64_t kSstatusRmask = kMstatusSie | kMstatusSpie | kMstatusSpp |
                                      kMstatusFs | kMstatusSum | kMstatusMxr |
                                      kMstatusUxlMask | (1ULL << 63);
static const uint64_t kSstatusWmask = kMstatusSie | kMstatusSpie | kMstatusSpp |
                                      kMstatusFs | kMstatusSum | kMstatusMxr;

static const uint64_t kMcountinhibitCy = 1ULL << 0;
static const uint64_t kMcountinhibitIr = 1ULL << 2;
static const uint64_t kMisaMxl64 = 2ULL << 62;  // MXL = 2: RV64
static const uint64_t kMisaA = 1ULL << 0;
static const uint64_t kMisaC = 1ULL << 2;
static const uint64_t kMisaD = 1ULL << 3;
static const uint64_t kMisaF = 1ULL << 5;  // misa bit letters: E=4, F=5
static const uint64_t kMisaI = 1ULL << 8;
static const uint64_t kMisaM = 1ULL << 12;
static const uint64_t kMisaS = 1ULL << 18;
static const uint64_t kMisaSupported =
    kMisaMxl64 | kMisaA | kMisaC | kMisaD | kMisaF | kMisaI | kMisaM |
    kMisaS;
// tcontrol MTE/MPTE (RISC-V debug spec, tcontrol register)
static const uint64_t kTcontrolMte = 1ULL << 7;
static const uint64_t kTcontrolMpte = 1ULL << 6;

// menvcfg/senvcfg (priv spec 3.7.2): the byte of CBIE/CBCFE/CBZE control
// fields, plus ADUE (Svadu hardware A/D update) and STCE (Sstc timecmp).
// PBMTE stays hardwired 0 — the DTB does not advertise Svpbmt.
static const uint64_t kMenvcfgWmask = 0xffULL | (1ULL << 61) | (1ULL << 63);
static const uint64_t kMenvcfgAdue = 1ULL << 61;
static const uint64_t kSenvcfgWmask = 0xffULL;

// MHPM counters 3..18, the set QEMU virt implements (OpenSBI banner
// "MHPM Info : 16 (0x0007fff8)").
enum { kMhpmFirst = 3, kMhpmCount = 16, kMhpmLast = 18 };

// Sdtrig (debug triggers): QEMU virt has 2 mcontrol6 triggers (banner
// "Debug Triggers : 2 triggers"); the type field sits in tdata1[63:60].
enum { kTrigCount = 2 };
static const uint64_t kTrigTypeMcontrol6 = 6ULL << 60;
static const uint64_t kTrigTypeMask = 0xfULL << 60;
// Bits outside type and dmode are WARL-stored; the match/action machinery
// is registered in 准则审查.md and lands with the gdb stub.
static const uint64_t kTrigWmask = ~(kTrigTypeMask | (1ULL << 59));

// Cache-block size for Zicboz/Zicbom (virt DTB riscv,cboz-block-size /
// cbom-block-size = 64).
enum { kCacheBlockSize = 64 };

// Interrupt causes, shared by mie/mip bit positions (priv spec 3.1.9; only
// the implemented set — U-level sources have no generators on these
// machines and H is absent).
static const uint64_t kIrqSsip = 1ULL << 1;
static const uint64_t kIrqMsip = 1ULL << 3;
static const uint64_t kIrqStip = 1ULL << 5;
static const uint64_t kIrqMtip = 1ULL << 7;
static const uint64_t kIrqSeip = 1ULL << 9;
static const uint64_t kIrqMeip = 1ULL << 11;
// mideleg WARL: only S-level interrupts are delegable (priv spec 3.1.10).
static const uint64_t kMidelegWmask = kIrqSsip | kIrqStip | kIrqSeip;
// mie/sie and mip/sip are views of the S-level subset.
static const uint64_t kIrqSLevelMask = kMidelegWmask;
// medeleg WARL: every standard exception except M-mode ecall (11) is
// delegable among codes 0..15 (priv spec 3.1.8; xiangshanNEMU
// MEDELEG_NONRVH agrees).
static const uint64_t kMedelegWmask = 0xf7ffULL;

// satp (priv spec 4.1.11): Bare and Sv39 are the implemented modes; the
// writable fields are mode, ASID and PPN.
static const uint64_t kSatpModeBare = 0;
static const uint64_t kSatpModeSv39 = 8;
static const uint64_t kSatpWmask =
    (0xfULL << 60) | (0xffffULL << 44) | ((1ULL << 44) - 1);

// PMP (priv spec 3.7): 16 entries like QEMU virt (OpenSBI banner "PMP Count
// : 16"), G = 2 (16-byte granularity, banner "PMP Granularity : 2 bits"),
// implemented physical address bits 54 (banner "PMP Address Bits : 54").
enum { kPmpCount = 16, kPmpAddrBits = 54 };
// pmpcfg byte fields (priv spec 3.7.1): the A field occupies bits 5:4.
enum {
  kPmpR = 1 << 0,
  kPmpW = 1 << 1,
  kPmpX = 1 << 2,
  kPmpAMask = 3 << 4,
  kPmpATor = 1 << 4,
  kPmpANa4 = 2 << 4,
  kPmpANapot = 3 << 4,
  kPmpL = 1 << 7,
};

// Exception causes (priv spec 1.3, table 1.2).
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
  kExSupervisorEcall = 9,
  kExMachineEcall = 11,
  kExFetchPageFault = 12,
  kExLoadPageFault = 13,
  kExStorePageFault = 14,
};

// Access classes for address translation and PMP checks. AMOs need both
// read and write permission.
enum {
  kAccIfetch = 0,
  kAccRead,
  kAccWrite,
  kAccAmo,
};

typedef struct RiscvState {
  uint64_t gpr[32];
  uint64_t fpr[32];
  uint64_t misa;
  uint64_t mstatus;
  uint64_t mie;
  uint64_t mip;       // software-writable part; external lines live in ext_irq
  uint64_t mtvec;
  uint64_t mscratch;
  uint64_t mepc;
  uint64_t mcause;
  uint64_t mtval;
  uint64_t medeleg;
  uint64_t mideleg;
  uint64_t mcounteren;
  uint64_t scounteren;
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
  uint64_t tdata[3];  // tdata1/2/3; tdata1 resets to an mcontrol6 type
  uint64_t menvcfg;
  uint64_t senvcfg;
  uint64_t stimecmp;  // Sstc: S-level timer compare, drives STIP
  uint64_t mhpmcounter[kMhpmCount];  // counters 3..18
  uint64_t mhpmevent[kMhpmCount];
  uint64_t pmpcfg[2];   // pmpcfg0/pmpcfg2, 8 entries each (RV64 layout)
  uint64_t pmpaddr[kPmpCount];
  uint8_t priv;
  uint8_t frm;
  uint8_t fflags;
  uint8_t counter_written;
  // Device-driven interrupt inputs (CLINT MSIP/MTIP, PLIC MEIP/SEIP); ORed
  // into mip for every observation, mirroring how wires drive the CSR bits.
  uint64_t ext_irq;
  int wait;  // wfi sleep: no fetch until (mip|ext_irq)&mie is nonzero
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
// Level input from machine interrupt controllers; bit is one of kIrq*.
void RiscvSetExtIrq(CpuState *cpu, uint64_t bit, int level);

// csr.c
int RiscvCsrRead(const CpuState *cpu, RiscvState *s, uint64_t addr,
                 uint64_t *out);
// next_pc is the address of the instruction that follows the CSR write; the
// misa WARL check needs it to reject C-clear that would misalign the next
// fetch (IALIGN=32).
int RiscvCsrWrite(RiscvState *s, uint64_t addr, uint64_t val, uint64_t next_pc);
void RiscvTrap(CpuState *cpu, RiscvState *s, uint64_t cause, uint64_t tval);
// Picks the highest-priority enabled pending interrupt per the priv spec
// (priority order MEI, MSI, MTI, SEI, SSI, STI; target-mode gating through
// delegation and the xIE stack) and injects it as a trap.
void RiscvDeliverPendingInterrupt(CpuState *cpu, RiscvState *s);
// xRET helpers return the target PC; the caller routes it through next_pc.
uint64_t RiscvMret(RiscvState *s);
uint64_t RiscvSret(RiscvState *s);

// mmu.c — address translation and PMP enforcement. Returns 0 and sets
// *paddr on success; on failure it raises the matching trap (page fault for
// translation failures, access fault for PMP) and returns -1.
int RiscvTranslate(CpuState *cpu, RiscvState *s, uint64_t vaddr, int acc,
                   uint64_t *paddr);
// PMP check for a physical range under a privilege mode; 0 = allowed.
int RiscvPmpAllowed(const RiscvState *s, uint64_t addr, uint64_t len, int acc,
                    int mode);

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
