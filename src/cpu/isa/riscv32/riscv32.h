#ifndef CEMU_CPU_ISA_RISCV32_RISCV32_H
#define CEMU_CPU_ISA_RISCV32_RISCV32_H

#include "bus/bus.h"
#include "cpu/isa/isa.h"
#include "cpu/isa/riscv32/platform.h"

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
// counter write overrides the implicit increment; rv32mi-p-instret_overflow
// anchors the minstret case).
enum {
  kCounterNone = 0,
  kCounterMcycle = 1,
  kCounterMinstret = 2,
};

// Privilege modes (priv spec 3.1.1).
enum { kPrivUser = 0, kPrivSupervisor = 1, kPrivHypervisor = 2, kPrivMachine = 3 };

// mstatus fields (priv spec 3.1.6). RV32 mstatus is 32 bits: there is no
// SXL/UXL field, and the SD summary lives in bit 31.
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
// Writable mstatus bits (xiangshanNEMU priv.c MSTATUS_WMASK_BASE 0x7e19aa |
// FS, plus MPRV/SUM/MXR/TVM/TW/TSR). RV32 has no UXL/MXL.
static const uint64_t kMstatusWmask =
    kMstatusSie | kMstatusMie | kMstatusSpie | kMstatusMpie | kMstatusSpp | kMstatusMppMask |
    kMstatusFs | kMstatusMprv | kMstatusSum | kMstatusMxr | kMstatusTvm | kMstatusTw | kMstatusTsr;
// sstatus view over mstatus (priv spec 3.1.6.3): readable = S-level fields +
// SD; XS is not implemented (no extension state) and reads 0.
static const uint64_t kSstatusRmask =
    kMstatusSie | kMstatusSpie | kMstatusSpp | kMstatusFs | kMstatusSum | kMstatusMxr;
static const uint64_t kSstatusWmask =
    kMstatusSie | kMstatusSpie | kMstatusSpp | kMstatusFs | kMstatusSum | kMstatusMxr;
// SD is mstatus[XLEN-1]: bit 31 on RV32 (priv spec 3.1.6.3).
static const uint64_t kSstatusSd = 1ULL << 31;

static const uint64_t kMcountinhibitCy = 1ULL << 0;
static const uint64_t kMcountinhibitIr = 1ULL << 2;
static const uint64_t kMisaMxl32 = 1ULL << 30;  // MXL = 1: RV32
static const uint64_t kMisaA = 1ULL << 0;
static const uint64_t kMisaC = 1ULL << 2;
static const uint64_t kMisaD = 1ULL << 3;
static const uint64_t kMisaF = 1ULL << 5;  // misa bit letters: E=4, F=5
static const uint64_t kMisaI = 1ULL << 8;
static const uint64_t kMisaM = 1ULL << 12;
static const uint64_t kMisaS = 1ULL << 18;
static const uint64_t kMisaU = 1ULL << 20;  // misa bit letters: S=18, U=20
// U=1 because user mode is implemented: ecall from U raises cause 8, sret can
// land in U, and the walk checks PTE.U against the effective privilege
// (priv spec 3.1.1 "U: User mode is implemented").
static const uint64_t kMisaSupported =
    kMisaMxl32 | kMisaA | kMisaC | kMisaD | kMisaF | kMisaI | kMisaM | kMisaS | kMisaU;
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

// Sdtrig (debug triggers): 2 mcontrol6 triggers, the QEMU virt set. tdata1 is
// XLEN bits, so the type field sits in the top four bits and dmode just below
// it: bits 31:28 / 27 on RV32 (QEMU target/riscv/debug.h MCONTROL_TYPE(xlen),
// MCONTROL_DMODE(xlen)). The remaining fields keep their low offsets:
// load[0] store[1] execute[2] u[3] s[4] m[6] match[10:7] chain[11]
// action[15:12].
enum { kTrigCount = 2 };
static const uint64_t kTrigTypeMcontrol6 = 6ULL << 28;
static const uint64_t kMc6Load = 1ULL << 0;
static const uint64_t kMc6Store = 1ULL << 1;
static const uint64_t kMc6Execute = 1ULL << 2;
static const uint64_t kMc6U = 1ULL << 3;
static const uint64_t kMc6S = 1ULL << 4;
static const uint64_t kMc6M = 1ULL << 6;
static const uint64_t kMc6Chain = 1ULL << 11;
static const uint64_t kMc6Dmode = 1ULL << 27;
// match encodings (debug spec table 5.4); the machine accepts only EQ/GE/LT
// (xiangshanNEMU mcontrol6_checked_write WARLs every other encoding to EQ).
enum { kMc6MatchEq = 0, kMc6MatchGe = 2, kMc6MatchLt = 3 };
// tdata3 (debug spec 5.3.10): sselect[1:0], svalue[33:2], sbytemask[39:36],
// mhselect[50:48], mhvalue[63:51]. tdata3 is XLEN bits: on RV32 only the
// low 32 bits are accessible, so svalue keeps its low 30 bits and the
// sbytemask/mh fields read 0 (every scontext byte significant, the default
// the RV64 matcher uses when sbytemask=0).
static const uint64_t kTd3SselectMask = 3ULL;
static const uint64_t kTd3SvalueShift = 2;
static const uint64_t kTd3SbytemaskShift = 36;
// Trigger operations: one check per operation class (debug spec 5.3).
enum { kTrigOpExecute = 1, kTrigOpStore = 2, kTrigOpLoad = 4 };

// Cache-block size for Zicboz/Zicbom (virt DTB riscv,cboz-block-size /
// cbom-block-size = 64).
enum { kCacheBlockSize = 64 };

// Interrupt causes are shared with the mip bit positions and live in
// platform.h — that vocabulary is also what a board wires its controllers to.
// mideleg WARL: only S-level interrupts are delegable (priv spec 3.1.10).
static const uint64_t kMidelegWmask = kIrqSsip | kIrqStip | kIrqSeip;
// mie/sie and mip/sip are views of the S-level subset.
static const uint64_t kIrqSLevelMask = kMidelegWmask;
// medeleg WARL: every standard exception except M-mode ecall (11) is
// delegable among codes 0..15 (priv spec 3.1.8; xiangshanNEMU
// MEDELEG_NONRVH agrees). medelegh exists on RV32 and reads 0 because no
// standard exception lives above bit 15.
static const uint64_t kMedelegWmask = 0xf7ffULL;

// satp (priv spec 4.1.11): RV32 layout — MODE[31], ASID[30:22], PPN[21:0] —
// and Bare/Sv32 are the implemented modes.
static const uint64_t kSatpModeBare = 0;
static const uint64_t kSatpModeSv32 = 1;
static const uint64_t kSatpWmask = (1ULL << 31) | (0x1ffULL << 22) | ((1ULL << 22) - 1);
static const uint64_t kSatpPpnMask = (1ULL << 22) - 1;
static const uint64_t kSatpAsidMask = 0x1ffULL;
static const uint64_t kSatpAsidShift = 22;

// Sv32 (priv spec 4.3.2 / 5.3): two levels, 4-byte PTEs, 10-bit VPNs, a
// 34-bit physical address space.
enum {
  kSv32Levels = 2,
  kSv32VpnBits = 10,
  kSv32PteSize = 4,
  kSv32PageShift = 12,
};
static const uint64_t kSv32VpnMask = (1ULL << kSv32VpnBits) - 1;
static const uint64_t kSv32OffsetMask = (1ULL << kSv32PageShift) - 1;
static const uint64_t kSv32SuperpageMask = (1ULL << (kSv32PageShift + kSv32VpnBits)) - 1;
// Sv32's physical address is 34 bits (priv spec 4.3.2): PPN[1:0] << 12.
static const uint64_t kSv32PhysSize = 1ULL << 34;

// The XLEN-width mask, for splitting or truncating a value at 32 bits.
static const uint64_t kXlenMask = 0xffffffffULL;

// PMP (priv spec 3.7): 16 entries like QEMU virt, G = 2 (16-byte
// granularity). RV32 packs four entries into each of pmpcfg0..3 and holds
// pmpaddr[33:2] in a 32-bit CSR.
enum { kPmpCount = 16 };

// pmpcfg byte fields (priv spec 3.7.1): the A field occupies bits 4:3.
enum {
  kPmpR = 1 << 0,
  kPmpW = 1 << 1,
  kPmpX = 1 << 2,
  kPmpAMask = 3 << 3,
  kPmpATor = 1 << 3,
  kPmpANa4 = 2 << 3,
  kPmpANapot = 3 << 3,
  kPmpL = 1 << 7,
};

// Interrupt causes carry this bit (priv spec 1.3; XLEN-1 on RV32).
static const uint64_t kIntBit = 0x80000000ULL;

// Exception causes (priv spec 1.3, table 1.2). 14 is reserved — store page
// fault is 15 (rv32si-p-dirty pins the value on this backend).
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
  kExStorePageFault = 15,
};

// Sv32 translation cache, mirroring the riscv64 backend: an entry records
// every context its decision depended on (satp, privilege, SUM/MXR, ADUE,
// PMP generation and sfence generation) and the host backing of a plain RAM
// page. A miss on any field simply refills. Permissions are per access class,
// so `acc` is part of the tag.
enum { kTlbSets = 512, kTlbWays = 2 };
typedef struct Riscv32TlbEntry {
  uint64_t vpn, pbase;
  uint8_t* host;
  uint64_t gen;       // sfence generation
  uint64_t satp;
  uint64_t mstatus;   // SUM|MXR
  uint64_t menvcfg;   // ADUE
  uint32_t pmp_gen;
  uint8_t priv, eff, acc, pad;
} Riscv32TlbEntry;

typedef struct Riscv32State {
  uint64_t misa;
  uint64_t mstatus;
  uint64_t mie;
  uint64_t mip;  // software-writable part; external lines live in ext_irq
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
  // Per-trigger tdata1/2/3 (Sdtrig: tselect indexes tdata1/2/3); both
  // triggers reset to an mcontrol6 type. tdata2 holds a 32-bit address.
  uint64_t tdata1[kTrigCount];
  uint64_t tdata2[kTrigCount];
  uint64_t tdata3[kTrigCount];
  uint64_t scontext;  // Sdtrig Sscontext: the trigger qualification context
  uint64_t menvcfg;
  uint64_t senvcfg;
  uint64_t stimecmp;                 // Sstc: S-level timer compare, drives STIP
  uint64_t mnstatus;                 // Smrnig: WARL-stored RNMI status (suite writes NMIE)
  uint64_t mhpmcounter[kMhpmCount];  // counters 3..18
  uint64_t mhpmevent[kMhpmCount];
  // RV32 PMP layout: pmpcfg0..3 hold four entry bytes each (priv spec 3.7.1);
  // pmpaddr holds address bits [33:2].
  uint32_t pmpcfg[4];
  uint32_t pmpaddr[kPmpCount];
  uint8_t priv;
  uint8_t frm;
  uint8_t fflags;
  uint8_t counter_written;
  // Device-driven interrupt inputs (CLINT MSIP/MTIP, PLIC MEIP/SEIP); ORed
  // into mip for every observation, mirroring how wires drive the CSR bits.
  uint64_t ext_irq;
  Riscv32TlbEntry tlb[kTlbSets * kTlbWays];
  uint64_t tlb_gen;   // sfence.vma bumps this
  uint32_t tlb_clock;  // 2-way replacement clock
  uint32_t pmp_gen;    // PMP CSR writes bump this
  int res_valid;  // the LR reservation (single hart)
  uint64_t res_addr;
  // gdb stub (stage 3.5): bumped on every delivered trap; signal is the gdb
  // mapping of the last cause (step.c, where the cause is in hand).
  uint64_t trap_seq;
  uint8_t trap_signal;
} Riscv32State;

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

void riscv32_init(CpuState* cpu);
void riscv32_step(CpuState* cpu);
void riscv32_dump_regs(const CpuState* cpu);
// The memory pipeline lives with the instruction set (exec.c): translation ->
// PMP -> bus, raising the matching trap on failure. The step protocol (step.c)
// and every load/store case share it.
uint64_t riscv32_mem_load(frame* f, uint64_t vaddr, int size, int acc);
void riscv32_mem_store(frame* f, uint64_t vaddr, int size, uint64_t val);
// Sdtrig trigger module (trigger.c): the check fires a breakpoint exception
// through the raise channel when an armed mcontrol6 matches; the two write
// helpers apply the tdata1/tdata3 WARL policies.
void Riscv32TriggerCheck(frame* f, Riscv32State* s, int op, uint64_t addr);
void Riscv32TriggerWriteTdata1(Riscv32State* s, uint64_t val);
void Riscv32TriggerWriteTdata3(Riscv32State* s, uint64_t val);
// Interrupt line input; installed as CpuState.set_irq so boards never name a
// CPU-model function. `bit` is one of kIrq* (platform.h).
void riscv32_set_ext_irq(CpuState* cpu, uint64_t bit, int level);

// The 32-bit register bank the interpreter runs on (exec.c). Step copies the
// committed CpuState bank in at entry and commits it back on success; a
// faulting instruction's writes are therefore rolled back as a whole, and C
// arithmetic on the bank wraps at 32 bits without a truncation at every case.
void Riscv32LoadRegs(CpuState* cpu);
void Riscv32StoreRegs(CpuState* cpu);

// csr.c
int riscv32_csr_read(const CpuState* cpu, Riscv32State* s, uint64_t addr, uint64_t* out);
// next_pc is the address of the instruction that follows the CSR write; the
// misa WARL check needs it to reject C-clear that would misalign the next
// fetch (IALIGN=32).
int riscv32_csr_write(Riscv32State* s, uint64_t addr, uint64_t val, uint64_t next_pc);
// Takes the trap and RETURNS the vector base; the caller decides where the
// pc goes (the run loop commits dnpc, interrupt injection sets cpu->pc).
uint64_t riscv32_trap(CpuState* cpu, Riscv32State* s, uint64_t cause, uint64_t tval);
// Picks the highest-priority enabled pending interrupt per the priv spec
// (priority order MEI, MSI, MTI, SEI, SSI, STI; target-mode gating through
// delegation and the xIE stack) and injects it as a trap into cpu->pc.
void riscv32_deliver_interrupt(CpuState* cpu, Riscv32State* s);
// Sstc: samples the host clock and drives the STIP line from stimecmp. Called
// at the board's batch boundaries (and every step while sleeping), not per
// instruction — the line is a timer input, not an instruction-stream fact.
void riscv32_sync_timer(CpuState* cpu, Riscv32State* s);
// xRET helpers return the target PC; the caller routes it through next_pc.
uint64_t riscv32_mret(Riscv32State* s);
uint64_t riscv32_sret(Riscv32State* s);

// mmu.c — Sv32 address translation and PMP enforcement. On failure it raises
// the matching trap through the frame (page fault for translation failures,
// access fault for PMP) and longjmps to the step landing pad; success
// returns the physical address through *paddr, and for a plain RAM page the
// host backing of its 4 KiB page through *host (NULL for devices/unmapped).
int Riscv32Translate(frame* f, Riscv32State* s, uint64_t vaddr, int size, int acc,
                     uint64_t* paddr, uint8_t** host);
// PMP check for a physical range under a privilege mode; 0 = allowed.
int Riscv32PmpAllowed(const Riscv32State* s, uint64_t addr, uint64_t len, int acc, int mode);

// fp.c
uint64_t Riscv32FpBinaryS(Riscv32State* s, uint64_t ra, uint64_t rb, int kind, int rm);
uint64_t Riscv32FpBinaryD(Riscv32State* s, uint64_t ra, uint64_t rb, int kind, int rm);
uint64_t Riscv32FpSqrtS(Riscv32State* s, uint64_t ra, int rm);
uint64_t Riscv32FpSqrtD(Riscv32State* s, uint64_t ra, int rm);
uint64_t Riscv32FpFmaS(Riscv32State* s, uint64_t ra, uint64_t rb, uint64_t rc, int rm);
uint64_t Riscv32FpFmaD(Riscv32State* s, uint64_t ra, uint64_t rb, uint64_t rc, int rm);
uint64_t Riscv32FpMinMaxS(Riscv32State* s, uint64_t ra, uint64_t rb, int is_max);
uint64_t Riscv32FpMinMaxD(Riscv32State* s, uint64_t ra, uint64_t rb, int is_max);
uint64_t Riscv32FpCmpS(Riscv32State* s, uint64_t ra, uint64_t rb, int kind);
uint64_t Riscv32FpCmpD(Riscv32State* s, uint64_t ra, uint64_t rb, int kind);
uint64_t Riscv32FpClassS(uint64_t ra);
uint64_t Riscv32FpClassD(uint64_t ra);
uint64_t Riscv32FpSgnjS(uint64_t ra, uint64_t rb, int kind);
uint64_t Riscv32FpSgnjD(uint64_t ra, uint64_t rb, int kind);
uint64_t Riscv32FpF2IS(Riscv32State* s, uint64_t ra, int to_signed, int dbits, int rm);
uint64_t Riscv32FpF2ID(Riscv32State* s, uint64_t ra, int to_signed, int dbits, int rm);
uint64_t Riscv32FpI2FS(Riscv32State* s, uint64_t val, int val_signed, int bits, int rm);
uint64_t Riscv32FpI2FD(Riscv32State* s, uint64_t val, int val_signed, int bits, int rm);
uint64_t Riscv32FpCvtSD(Riscv32State* s, uint64_t ra, int rm);  // double -> single
uint64_t Riscv32FpCvtDS(Riscv32State* s, uint64_t ra, int rm);  // single -> double

#endif
