// The riscv32 instruction set: fetch, decode, execute — one C scope per
// instruction, a nested switch per manual chapter (opcode -> funct3 ->
// funct7), each case reading like the spec's own pseudocode
// (x[rd] = x[rs1] + x[rs2]; pc = pc + imm). C operators do the arithmetic;
// helpers exist only for what C has no operator for (memory translation,
// CSRs, FP, traps). Names follow the unprivileged/privileged specs.
//
// The register bank is a 32-bit array (exec.h): C arithmetic wraps at XLEN
// without a truncation at every case, and step.c commits it back to CpuState
// only when the instruction retires.
//
// RV32IMAFDC + Zicsr/Zifencei/Zicbom/Zicboz. RV64-only encodings (LD/SD/LWU,
// OP-IMM-32, OP-32, LR.D/SC.D/AMO*.D, FMV.X.D/FMV.D.X, FCVT.L/LU) fall to the
// default illegal case; the RV32-only compressed forms (C.JAL, C.FLW/C.FSW,
// C.FLWSP/C.FSWSP) are decoded here.
//
// The step protocol around this switch (interrupt delivery, fetch, the single
// commit) lives in step.c.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/isa/isa.h"
#include "debug/debug.h"
#include "host/host.h"
#include "util/log.h"

#include "exec.h"  // last: it defines the short register macros x/f/rs

// Fixed names: `rv32_cpu` (CpuState), `rs` (Riscv32State), `fr` (step
// frame). The banks and their manual-notation aliases (x, f) come from
// exec.h.
CpuState* rv32_cpu;
Riscv32State* rv32_rs;
uint32_t rv32_x[32];

// Step entry/exit: the local bank is the architectural register file while an
// instruction executes. The copy in is unconditional (a debugger may have
// written the committed bank between steps); the copy out happens only when
// the instruction retires, which is what rolls a faulting instruction's
// register writes back (instruction atomicity).
void Riscv32LoadRegs(CpuState* c) {
  for (int i = 0; i < 32; i++) rv32_x[i] = (uint32_t)c->gpr[i];
  rv32_x[0] = 0;
}

void Riscv32StoreRegs(CpuState* c) {
  for (int i = 1; i < 32; i++) c->gpr[i] = rv32_x[i];
  c->gpr[0] = 0;
}

// ---- instruction fields (volume I, figure 2.2) ------------------------------

// Field accessors are macros: they keep every call site identical to the
// spec's own notation (x[rd(i)]) and cannot collide with libc declarations.
#define rd(i) ((int)(((i) >> 7) & 31))
#define rs1(i) ((int)(((i) >> 15) & 31))
#define rs2(i) ((int)(((i) >> 20) & 31))
#define rs3(i) ((int)((i) >> 27))
#define f3(i) ((int)(((i) >> 12) & 7))
#define f7(i) ((int)((i) >> 25))

static int32_t sext32(uint32_t v, int bits) {
  uint32_t m = 1u << (bits - 1);
  return (int32_t)((v ^ m) - m);
}

static int32_t imm_i(uint32_t i) { return sext32(i >> 20, 12); }
static int32_t imm_s(uint32_t i) { return sext32(((i >> 25) << 5) | ((i >> 7) & 31), 12); }
static int32_t imm_b(uint32_t i) {
  return sext32((((i >> 31) & 1) << 12) | (((i >> 7) & 1) << 11) | (((i >> 25) & 0x3f) << 5) |
                    (((i >> 8) & 15) << 1),
                13);
}
static uint32_t imm_u(uint32_t i) { return i & 0xfffff000u; }
static int32_t imm_j(uint32_t i) {
  return sext32((((i >> 31) & 1) << 20) | (((i >> 12) & 0xff) << 12) | (((i >> 20) & 1) << 11) |
                    (((i >> 21) & 0x3ff) << 1),
                21);
}

_Noreturn static void illegal(frame* fr, uint64_t tval) { raise_(fr, kExIllegal, tval); }

// ---- memory pipeline: translation -> PMP -> unmapped-physical -> bus ---------

static int fault_cause(int acc) {
  if (acc == acc_ifetch) return kExFetchFault;
  return acc == acc_write ? kExStoreFault : kExLoadFault;
}

uint64_t riscv32_mem_load(frame* fr, uint64_t vaddr, int size, int acc) {
  // Load triggers fire on the effective address before the access happens,
  // so even an access that would fault triggers first (xiangshanNEMU checks
  // in the ldst templates before the rtl memory op). Fetches don't qualify.
  if (acc != acc_ifetch) Riscv32TriggerCheck(fr, rs, kTrigOpLoad, vaddr);
  uint64_t paddr;
  Riscv32Translate(fr, rs, vaddr, acc, &paddr);  // raises the page fault itself
  BusRegion* r;
  if (!Riscv32PmpAllowed(rs, paddr, (uint64_t)size, acc, rs->priv) ||
      BusProbe(rv32_cpu->bus, paddr, size, &r) != 0)
    raise_(fr, fault_cause(acc), vaddr);
  uint64_t v = BusRead(rv32_cpu->bus, paddr, size);
  // The bus category reports device accesses (MMIO and port hits); an
  // instruction fetch is not one — see the x86 funnel's is_fetch.
  if (DebugOn(kDbgBus) && !r->host && acc != acc_ifetch)
    DebugBus(fr, r->ops->name, paddr, size, 1, v);
  if (DebugOn(kDbgMem)) DebugMem(fr, vaddr, size, acc, v, 1);
  return v;
}

void riscv32_mem_store(frame* fr, uint64_t vaddr, int size, uint64_t val) {
  Riscv32TriggerCheck(fr, rs, kTrigOpStore, vaddr);  // before the store commits
  uint64_t paddr;
  Riscv32Translate(fr, rs, vaddr, acc_write, &paddr);
  BusRegion* r;
  if (!Riscv32PmpAllowed(rs, paddr, (uint64_t)size, acc_write, rs->priv) ||
      BusProbe(rv32_cpu->bus, paddr, size, &r) != 0)
    raise_(fr, kExStoreFault, vaddr);
  if (DebugOn(kDbgBus) && !r->host) {
    DebugBus(fr, r->ops->name, paddr, size, 0, val);
  } else if (DebugOn(kDbgMem)) {
    DebugMem(fr, vaddr, size, acc_write, val, 0);
  }
  BusWrite(rv32_cpu->bus, paddr, size, val);
}

// Loads at the manual's widths: lb/lh/lw sign-extend into the 32-bit
// register, the u forms don't.
static uint32_t load32(frame* fr, uint32_t addr, int size, int sext_w) {
  uint64_t v = riscv32_mem_load(fr, addr, size, acc_read);
  if (!sext_w) return (uint32_t)v;
  switch (size) {
    case 1:
      return (uint32_t)(int32_t)(int8_t)v;
    case 2:
      return (uint32_t)(int32_t)(int16_t)v;
    default:
      return (uint32_t)v;
  }
}

// ---- Zicsr (volume II): the immediate forms carry the source in rs1[4:0] ------

static void csr_op(frame* fr, uint32_t i) {
  int f3v = f3(i);
  uint64_t csr = (i >> 20) & 0xfff;
  // Zicsr names by f3 (1/2/3 register, 5/6/7 immediate forms).
  static const char* const kCsrNames[8] = {0,    "csrrw",  "csrrs",  "csrrcs",
                                           0,    "csrrwi", "csrrsi", "csrrci"};
  fr->rec.mnemonic = kCsrNames[f3v];
  uint64_t old;
  if (riscv32_csr_read(rv32_cpu, rs, csr, &old) != 0) illegal(fr, i);
  int imm_form = f3v >= 5;
  uint64_t src = imm_form ? (uint64_t)rs1(i) : x[rs1(i)];
  // Zicsr (volume II): csrrw always writes; csrrs/csrrc write only when the
  // source is nonzero — register forms test rs1 = x0, immediate forms test
  // uimm = 0. That is what makes the read-only idiom (csrr t0, csr) work.
  int do_write = 1;
  uint64_t wval = old;
  switch (f3v & 3) {
    case 1:
      wval = src;
      break;
    case 2:
      wval = old | src;
      do_write = src != 0;
      break;
    case 3:
      wval = old & ~src;
      do_write = src != 0;
      break;
    default:
      illegal(fr, i);
  }
  // next_pc is the instruction after this one; the misa WARL check needs it.
  if (do_write && riscv32_csr_write(rs, csr, wval, (uint32_t)(rv32_cpu->pc + 4)) != 0) illegal(fr, i);
  x[rd(i)] = (uint32_t)old;
}

// ---- system (priv spec: ecall/ebreak/mret/sret/wfi/sfence) -------------------

static uint64_t system_op(frame* fr, uint32_t i) {
  uint32_t imm12 = (i >> 20) & 0xfff;
  switch (imm12) {
    case 0x000:  // ecall: the cause follows the active privilege (spec 1.4)
      fr->rec.mnemonic = "ecall";
      raise_(fr,
             rs->priv == kPrivMachine      ? kExMachineEcall
             : rs->priv == kPrivSupervisor ? kExSupervisorEcall
                                           : kExUserEcall,
             0);
    case 0x001:  // ebreak: mtval is the breakpoint address
      fr->rec.mnemonic = "ebreak";
      raise_(fr, kExBreakpoint, rv32_cpu->pc);
    case 0x302:  // mret
      fr->rec.mnemonic = "mret";
      if (rd(i) || rs1(i) || rs->priv != kPrivMachine) illegal(fr, i);
      return riscv32_mret(rs);
    case 0x102:  // sret
      fr->rec.mnemonic = "sret";
      if (rd(i) || rs1(i)) illegal(fr, i);
      // TSR: S-mode sret traps (priv spec 3.1.6.8); M-mode executes it.
      if (rs->priv == kPrivSupervisor && (rs->mstatus & kMstatusTsr)) illegal(fr, i);
      if (rs->priv < kPrivSupervisor) illegal(fr, i);
      return riscv32_sret(rs);
    case 0x105:  // wfi
      fr->rec.mnemonic = "wfi";
      if (rd(i) || rs1(i)) illegal(fr, i);
      // TW: S-mode wfi traps (priv spec 3.1.6.8); U-mode always traps.
      if (rs->priv == kPrivUser || (rs->priv == kPrivSupervisor && (rs->mstatus & kMstatusTw)))
        illegal(fr, i);
      // Sleep with no fetch until an enabled interrupt arrives; the machine
      // loop keeps the timers ticking. With one already pending: a nop.
      if (((rs->mip | rs->ext_irq) & rs->mie) == 0) rv32_cpu->wait = 1;
      return (uint32_t)(rv32_cpu->pc + 4);
    case 0x120:  // sfence.vma
      fr->rec.mnemonic = "sfence.vma";
      if (rd(i)) illegal(fr, i);
      // TVM: S-mode fences trap (priv spec 3.1.6.8); U-mode always traps.
      // No translation cache exists: every access walks the tables.
      if (rs->priv == kPrivUser || (rs->priv == kPrivSupervisor && (rs->mstatus & kMstatusTvm)))
        illegal(fr, i);
      return (uint32_t)(rv32_cpu->pc + 4);
    default:
      illegal(fr, i);
  }
  return (uint32_t)(rv32_cpu->pc + 4);
}

// ---- atomics (volume I, A extension; RV32 has the .w forms only) --------------

static void amo_op(frame* fr, uint32_t i) {
  int op5 = f7(i) >> 2;  // funct5 = inst[31:27] (volume I table A-2)
  uint32_t addr = x[rs1(i)];
  if (addr % 4) raise_(fr, kExStoreMisaligned, addr);
  uint32_t old, val, stored;
  switch (op5) {
    case 0x02:  // lr.w
      fr->rec.mnemonic = "lr.w";
      old = (uint32_t)riscv32_mem_load(fr, addr, 4, acc_read);
      rs->res_valid = 1;
      rs->res_addr = addr;
      x[rd(i)] = old;
      return;
    case 0x03:  // sc.w
      fr->rec.mnemonic = "sc.w";
      val = x[rs2(i)];
      if (rs->res_valid && rs->res_addr == addr) {
        riscv32_mem_store(fr, addr, 4, val);
        x[rd(i)] = 0;
      } else {
        x[rd(i)] = 1;
      }
      rs->res_valid = 0;
      return;
    case 0x01:
    case 0x00:
    case 0x04:
    case 0x0C:
    case 0x08:
    case 0x10:
    case 0x14:
    case 0x18:
    case 0x1C: {
      old = (uint32_t)riscv32_mem_load(fr, addr, 4, acc_read);
      val = x[rs2(i)];
      switch (op5) {
        case 0x01:
          fr->rec.mnemonic = "amoswap.w";
          stored = val;
          break;
        case 0x00:
          fr->rec.mnemonic = "amoadd.w";
          stored = old + val;
          break;
        case 0x04:
          fr->rec.mnemonic = "amoxor.w";
          stored = old ^ val;
          break;
        case 0x0C:
          fr->rec.mnemonic = "amoand.w";
          stored = old & val;
          break;
        case 0x08:
          fr->rec.mnemonic = "amoor.w";
          stored = old | val;
          break;
        case 0x10:
          fr->rec.mnemonic = "amomin.w";
          stored = (int32_t)old < (int32_t)val ? old : val;
          break;
        case 0x14:
          fr->rec.mnemonic = "amomax.w";
          stored = (int32_t)old > (int32_t)val ? old : val;
          break;
        case 0x18:
          fr->rec.mnemonic = "amominu.w";
          stored = old < val ? old : val;
          break;
        default:
          fr->rec.mnemonic = "amomaxu.w";
          stored = old > val ? old : val;
          break;
      }
      riscv32_mem_store(fr, addr, 4, stored);
      x[rd(i)] = old;
      return;
    }
    default:
      illegal(fr, i);
  }
}

// ---- floating point (fp.c is the IEEE-754 semantic backend) --------------------
// mstatus.FS gates every FP instruction and flips to Dirty on use (priv spec
// 3.1.6.7). rm = 7 (dyn) resolves through frm.

static int fp_rm(int rm_field) { return rm_field == kRmDyn ? rs->frm : rm_field; }

_Noreturn static void fp_disabled(frame* fr, uint32_t i) { raise_(fr, kExIllegal, i); }

static void fp_enter(frame* fr, uint32_t i) {
  if (!(rs->mstatus & kMstatusFs)) fp_disabled(fr, i);
  rs->mstatus |= kMstatusFs;
}

static void fp_op(frame* fr, uint32_t i) {
  fp_enter(fr, i);
  int f3v = f3(i), f7v = f7(i);
  int dbl = f7v & 1;
  int rm = fp_rm(f3v);
  uint64_t a = f[rs1(i)], b = f[rs2(i)], r = 0;
  switch (f7v) {
    case 0x00:
    case 0x01:
    case 0x04:
    case 0x05:
    case 0x08:
    case 0x09:
    case 0x0C:
    case 0x0D: {
      // fadd/fsub/fmul/fdiv (volume I, chapter 8): f7[4:3] picks the op.
      static const int kOps[4] = {kFpAdd, kFpSub, kFpMul, kFpDiv};
      static const char* const kF2Names[8] = {"fadd.s", "fadd.d", "fsub.s", "fsub.d",
                                              "fmul.s", "fmul.d", "fdiv.s", "fdiv.d"};
      int kind = kOps[(f7v >> 2) & 3];
      fr->rec.mnemonic = kF2Names[((f7v >> 2) & 3) * 2 + dbl];
      r = dbl ? Riscv32FpBinaryD(rs, a, b, kind, rm) : Riscv32FpBinaryS(rs, a, b, kind, rm);
      break;
    }
    case 0x2C:
    case 0x2D:  // fsqrt: rs2 must be zero (volume I)
      if (rs2(i)) illegal(fr, i);
      fr->rec.mnemonic = dbl ? "fsqrt.d" : "fsqrt.s";
      r = dbl ? Riscv32FpSqrtD(rs, a, rm) : Riscv32FpSqrtS(rs, a, rm);
      break;
    case 0x10:
    case 0x11: {  // fsgnj/fsgnjn/fsgnjx by f3 (gem5 decoder.isa)
      if (f3v > 2) illegal(fr, i);
      static const char* const kSgnjNames[6] = {"fsgnj.s", "fsgnj.d",  "fsgnjn.s",
                                                "fsgnjn.d", "fsgnjx.s", "fsgnjx.d"};
      fr->rec.mnemonic = kSgnjNames[f3v * 2 + dbl];
      r = dbl ? Riscv32FpSgnjD(a, b, f3v) : Riscv32FpSgnjS(a, b, f3v);
      break;
    }
    case 0x14:
    case 0x15: {  // fmin (f3=0) / fmax (f3=1)
      if (f3v > 1) illegal(fr, i);
      static const char* const kMinMaxNames[4] = {"fmin.s", "fmin.d", "fmax.s", "fmax.d"};
      fr->rec.mnemonic = kMinMaxNames[f3v * 2 + dbl];
      r = dbl ? Riscv32FpMinMaxD(rs, a, b, f3v) : Riscv32FpMinMaxS(rs, a, b, f3v);
      break;
    }
    case 0x50:
    case 0x51: {  // fle (f3=0) / flt (f3=1) / feq (f3=2)
      if (f3v > 2) illegal(fr, i);
      static const char* const kCmpNames[6] = {"fle.s", "fle.d", "flt.s",
                                               "flt.d", "feq.s", "feq.d"};
      fr->rec.mnemonic = kCmpNames[f3v * 2 + dbl];
      int kind = f3v == 0 ? kFpLe : f3v == 1 ? kFpLt : kFpEq;
      r = dbl ? Riscv32FpCmpD(rs, a, b, kind) : Riscv32FpCmpS(rs, a, b, kind);
      x[rd(i)] = (uint32_t)r;
      return;
    }
    case 0x60:
    case 0x61: {  // fcvt.{w|wu}.{s|d}: rs2 = 0:w 1:wu; 2:l/3:lu are RV64-only
      if (rs2(i) > 1) illegal(fr, i);
      static const char* const kF2INames[4] = {"fcvt.w.s", "fcvt.w.d", "fcvt.wu.s", "fcvt.wu.d"};
      int to_signed = (rs2(i) & 1) == 0;
      fr->rec.mnemonic = kF2INames[rs2(i) * 2 + dbl];
      uint64_t v = dbl ? Riscv32FpF2ID(rs, a, to_signed, 32, rm)
                       : Riscv32FpF2IS(rs, a, to_signed, 32, rm);
      x[rd(i)] = (uint32_t)v;
      return;
    }
    case 0x68:
    case 0x69: {  // fcvt.{s|d}.{w|wu}: 2:l/3:lu are RV64-only
      if (rs2(i) > 1) illegal(fr, i);
      static const char* const kI2FNames[4] = {"fcvt.s.w", "fcvt.d.w", "fcvt.s.wu", "fcvt.d.wu"};
      int to_signed = (rs2(i) & 1) == 0;
      fr->rec.mnemonic = kI2FNames[rs2(i) * 2 + dbl];
      r = dbl ? Riscv32FpI2FD(rs, x[rs1(i)], to_signed, 32, rm)
              : Riscv32FpI2FS(rs, x[rs1(i)], to_signed, 32, rm);
      break;
    }
    case 0x20:  // fcvt.s.d: rs2 = 1
      if (rs2(i) != 1) illegal(fr, i);
      fr->rec.mnemonic = "fcvt.s.d";
      r = Riscv32FpCvtSD(rs, a, rm);
      break;
    case 0x21:  // fcvt.d.s: rs2 = 0
      if (rs2(i) != 0) illegal(fr, i);
      fr->rec.mnemonic = "fcvt.d.s";
      r = Riscv32FpCvtDS(rs, a, rm);
      break;
    case 0x70: {  // fmv.x.w (f3=0) / fclass.s (f3=1)
      if (rs2(i) || f3v > 1) illegal(fr, i);
      fr->rec.mnemonic = f3v == 0 ? "fmv.x.w" : "fclass.s";
      // XLEN=32: the low 32-bit pattern is the whole register.
      x[rd(i)] = f3v == 0 ? (uint32_t)a : (uint32_t)Riscv32FpClassS(a);
      return;
    }
    case 0x71: {  // fclass.d (f3=1); fmv.x.d (f3=0) is RV64-only
      if (rs2(i) || f3v != 1) illegal(fr, i);
      fr->rec.mnemonic = "fclass.d";
      x[rd(i)] = (uint32_t)Riscv32FpClassD(a);
      return;
    }
    case 0x78:  // fmv.w.x: boxes the 32-bit pattern (NaN boxing, volume I)
      if (rs2(i) || f3v) illegal(fr, i);
      fr->rec.mnemonic = "fmv.w.x";
      f[rd(i)] = 0xffffffff00000000ULL | (x[rs1(i)] & 0xffffffffULL);
      return;
    case 0x79:  // fmv.d.x is RV64-only
      illegal(fr, i);
    default:
      illegal(fr, i);
  }
  f[rd(i)] = r;
}

// FMAs (volume I, chapter 8): fmsub subtracts the addend, fnmsub negates
// both, fnmadd negates the product; rs3 is the third source.
static void fma_op(frame* fr, uint32_t i, uint32_t opcode) {
  fp_enter(fr, i);
  int dbl = (i >> 25) & 1;
  int rm = fp_rm(f3(i));
  // FMAdd/FMSub/FNMSub/FNMAdd by opcode (volume I encode map), .s/.d by f7[0].
  static const char* const kFmaNames[8] = {"fmadd.s", "fmadd.d", "fmsub.s", "fmsub.d",
                                           "fnmsub.s", "fnmsub.d", "fnmadd.s", "fnmadd.d"};
  fr->rec.mnemonic = kFmaNames[((opcode >> 2) & 3) * 2 + dbl];
  uint64_t a = f[rs1(i)], b = f[rs2(i)], c = f[rs3(i)];
  // Sign flips by opcode (volume I encode map).
  if (opcode == 0x47 || opcode == 0x4f)  // fmsub, fnmadd: negate the addend
    c ^= dbl ? 0x8000000000000000ULL : 0x0000000080000000ULL;
  if (opcode == 0x4b || opcode == 0x4f)  // fnmsub, fnmadd: negate the product
    a ^= dbl ? 0x8000000000000000ULL : 0x0000000080000000ULL;
  f[rd(i)] = dbl ? Riscv32FpFmaD(rs, a, b, c, rm) : Riscv32FpFmaS(rs, a, b, c, rm);
}

// flw/fld/fsw/fsd: loads box the single-precision pattern; stores take the
// raw bits (volume I, NaN boxing — FLEN=64 with D on RV32 too).
static void fp_ldst(frame* fr, uint32_t i, int is_store, int is_double) {
  fr->rec.mnemonic = is_store ? (is_double ? "fsd" : "fsw") : (is_double ? "fld" : "flw");
  fp_enter(fr, i);
  int size = is_double ? 8 : 4;
  uint32_t addr = x[rs1(i)] + (uint32_t)(is_store ? imm_s(i) : imm_i(i));
  if (is_store) {
    riscv32_mem_store(fr, addr, size, f[rs2(i)]);
  } else {
    uint64_t v = riscv32_mem_load(fr, addr, size, acc_read);
    f[rd(i)] = is_double ? v : 0xffffffff00000000ULL | (uint32_t)v;
  }
}

// cbo.clean/inval/flush (Zicbom): no cache exists, so only the permission
// (U-mode needs the senvcfg R bit) and alignment checks remain (gem5 cbo).
static void cbo_op(frame* fr, uint32_t i) {
  fr->rec.mnemonic = f7(i) == 1 ? "cbo.clean" : f7(i) == 2 ? "cbo.flush" : "cbo.inval";
  if (rs->priv == kPrivUser && !(rs->senvcfg & (1ULL << 4))) illegal(fr, i);
  if (x[rs1(i)] % kCacheBlockSize) illegal(fr, i);
}

// cbo.zero (Zicboz): stores zeros over the whole cache block.
static void cbo_zero(frame* fr, uint32_t i) {
  fr->rec.mnemonic = "cbo.zero";
  if (rs->priv == kPrivUser && !(rs->senvcfg & (1ULL << 7))) illegal(fr, i);
  uint32_t addr = x[rs1(i)];
  if (addr % kCacheBlockSize) raise_(fr, kExStoreMisaligned, addr);
  for (int off = 0; off < kCacheBlockSize; off += 8)
    riscv32_mem_store(fr, (uint32_t)(addr + (uint32_t)off), 8, 0);
}

uint64_t riscv32_exec_inst(frame* fr, uint32_t i) {
  uint32_t pc = (uint32_t)rv32_cpu->pc;
  switch (i & 0x7f) {
    case 0x37:  // lui: x[rd] = sext(immediate[31:12])
      fr->rec.mnemonic = "lui";
      x[rd(i)] = imm_u(i);
      break;

    case 0x17:  // auipc: x[rd] = pc + sext(immediate[31:12])
      fr->rec.mnemonic = "auipc";
      x[rd(i)] = pc + imm_u(i);
      break;

    case 0x6f: {  // jal: x[rd] = pc+4; pc += imm_j
      fr->rec.mnemonic = "jal";
      uint32_t ret = pc + 4;
      x[rd(i)] = ret;
      return (uint32_t)(pc + (uint32_t)imm_j(i));
    }

    case 0x67: {  // jalr: t = (x[rs1] + imm) & ~1; x[rd] = pc+4; pc = t
      fr->rec.mnemonic = "jalr";
      uint32_t target = (x[rs1(i)] + (uint32_t)imm_i(i)) & ~1u;
      x[rd(i)] = pc + 4;
      return (rs->misa & kMisaC) ? target : (target & ~2u);
    }

    case 0x63: {  // branches: if (cond) pc += imm_b
      int32_t a = (int32_t)x[rs1(i)], b = (int32_t)x[rs2(i)];
      int taken;
      switch (f3(i)) {
        case 0:
          fr->rec.mnemonic = "beq";
          taken = x[rs1(i)] == x[rs2(i)];
          break;  // beq
        case 1:
          fr->rec.mnemonic = "bne";
          taken = x[rs1(i)] != x[rs2(i)];
          break;  // bne
        case 4:
          fr->rec.mnemonic = "blt";
          taken = a < b;
          break;  // blt
        case 5:
          fr->rec.mnemonic = "bge";
          taken = a >= b;
          break;  // bge
        case 6:
          fr->rec.mnemonic = "bltu";
          taken = x[rs1(i)] < x[rs2(i)];
          break;  // bltu
        case 7:
          fr->rec.mnemonic = "bgeu";
          taken = x[rs1(i)] >= x[rs2(i)];
          break;  // bgeu
        default:
          illegal(fr, i);
          taken = 0;
      }
      if (taken) return (uint32_t)(pc + (uint32_t)imm_b(i));
      break;
    }

    case 0x03: {  // loads (volume I table 2-1; RV32 has no ld/lwu)
      uint32_t addr = x[rs1(i)] + (uint32_t)imm_i(i);
      switch (f3(i)) {
        case 0:
          fr->rec.mnemonic = "lb";
          x[rd(i)] = load32(fr, addr, 1, 1);
          break;  // lb
        case 1:
          fr->rec.mnemonic = "lh";
          x[rd(i)] = load32(fr, addr, 2, 1);
          break;  // lh
        case 2:
          fr->rec.mnemonic = "lw";
          x[rd(i)] = load32(fr, addr, 4, 1);
          break;  // lw
        case 4:
          fr->rec.mnemonic = "lbu";
          x[rd(i)] = load32(fr, addr, 1, 0);
          break;  // lbu
        case 5:
          fr->rec.mnemonic = "lhu";
          x[rd(i)] = load32(fr, addr, 2, 0);
          break;  // lhu
        default:
          illegal(fr, i);  // f3 3 (ld) and 6 (lwu) are RV64-only
      }
      break;
    }

    case 0x23: {  // stores (volume I table 2-2; RV32 has no sd)
      uint32_t addr = x[rs1(i)] + (uint32_t)imm_s(i);
      switch (f3(i)) {
        case 0:
          fr->rec.mnemonic = "sb";
          riscv32_mem_store(fr, addr, 1, x[rs2(i)]);
          break;  // sb
        case 1:
          fr->rec.mnemonic = "sh";
          riscv32_mem_store(fr, addr, 2, x[rs2(i)]);
          break;  // sh
        case 2:
          fr->rec.mnemonic = "sw";
          riscv32_mem_store(fr, addr, 4, x[rs2(i)]);
          break;  // sw
        default:
          illegal(fr, i);  // f3 3 (sd) is RV64-only
      }
      break;
    }

    case 0x13: {  // op-imm (volume I table 5-1)
      uint32_t a = x[rs1(i)];
      uint32_t imm = (uint32_t)imm_i(i);
      switch (f3(i)) {
        case 0:
          fr->rec.mnemonic = "addi";
          x[rd(i)] = a + imm;
          break;  // addi
        case 2:
          fr->rec.mnemonic = "slti";
          x[rd(i)] = (int32_t)a < (int32_t)imm;
          break;  // slti
        case 3:
          fr->rec.mnemonic = "sltiu";
          x[rd(i)] = a < imm;
          break;  // sltiu
        case 4:
          fr->rec.mnemonic = "xori";
          x[rd(i)] = a ^ imm;
          break;  // xori
        case 6:
          fr->rec.mnemonic = "ori";
          x[rd(i)] = a | imm;
          break;  // ori
        case 7:
          fr->rec.mnemonic = "andi";
          x[rd(i)] = a & imm;
          break;  // andi
        case 1: {  // slli: RV32 shamt is 5 bits; inst[25] is funct7 and must be 0
          if (f7(i)) illegal(fr, i);
          fr->rec.mnemonic = "slli";
          x[rd(i)] = a << rs2(i);
          break;
        }
        case 5: {  // srli / srai by inst[30]
          int f7v = f7(i);
          if (f7v != 0x00 && f7v != 0x20) illegal(fr, i);
          fr->rec.mnemonic = f7v == 0x20 ? "srai" : "srli";
          x[rd(i)] = f7v == 0x20 ? (uint32_t)((int32_t)a >> rs2(i)) : a >> rs2(i);
          break;
        }
      }
      break;
    }

    case 0x33: {  // op (volume I table 5-4; f7=0x01 is the M extension)
      uint32_t a = x[rs1(i)], b = x[rs2(i)];
      int f7v = f7(i);
      switch (f3(i)) {
        case 0:
          fr->rec.mnemonic =
              f7v == 0x00 ? "add" : f7v == 0x20 ? "sub" : f7v == 0x01 ? "mul" : NULL;
          if (f7v == 0x00)
            x[rd(i)] = a + b;  // add
          else if (f7v == 0x20)
            x[rd(i)] = a - b;  // sub
          else if (f7v == 0x01)
            x[rd(i)] = a * b;  // mul
          else
            illegal(fr, i);
          break;
        case 1:
          fr->rec.mnemonic = f7v == 0x00 ? "sll" : f7v == 0x01 ? "mulh" : NULL;
          if (f7v == 0x00)
            x[rd(i)] = a << (b & 31);  // sll
          else if (f7v == 0x01)        // mulh: high 32 of the signed product
            x[rd(i)] = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32);
          else
            illegal(fr, i);
          break;
        case 2:
          fr->rec.mnemonic = f7v == 0x00 ? "slt" : f7v == 0x01 ? "mulhsu" : NULL;
          if (f7v == 0x00)
            x[rd(i)] = (int32_t)a < (int32_t)b;  // slt
          else if (f7v == 0x01)                     // mulhsu
            x[rd(i)] = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(uint64_t)b) >> 32);
          else
            illegal(fr, i);
          break;
        case 3:
          fr->rec.mnemonic = f7v == 0x00 ? "sltu" : f7v == 0x01 ? "mulhu" : NULL;
          if (f7v == 0x00)
            x[rd(i)] = a < b;  // sltu
          else if (f7v == 0x01)  // mulhu
            x[rd(i)] = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
          else
            illegal(fr, i);
          break;
        case 4:
          fr->rec.mnemonic = f7v == 0x00 ? "xor" : f7v == 0x01 ? "div" : NULL;
          if (f7v == 0x00)
            x[rd(i)] = a ^ b;      // xor
          else if (f7v == 0x01) {  // div (signed; volume I M table)
            int32_t sa = (int32_t)a, sb = (int32_t)b;
            if (sb == 0)
              x[rd(i)] = ~0u;
            else if (sa == INT32_MIN && sb == -1)
              x[rd(i)] = (uint32_t)sa;
            else
              x[rd(i)] = (uint32_t)(sa / sb);
          } else
            illegal(fr, i);
          break;
        case 5:
          fr->rec.mnemonic =
              f7v == 0x00 ? "srl" : f7v == 0x20 ? "sra" : f7v == 0x01 ? "divu" : NULL;
          if (f7v == 0x00)
            x[rd(i)] = a >> (b & 31);  // srl
          else if (f7v == 0x20)
            x[rd(i)] = (uint32_t)((int32_t)a >> (b & 31));  // sra
          else if (f7v == 0x01)                             // divu
            x[rd(i)] = b ? a / b : ~0u;
          else
            illegal(fr, i);
          break;
        case 6:
          fr->rec.mnemonic = f7v == 0x00 ? "or" : f7v == 0x01 ? "rem" : NULL;
          if (f7v == 0x00)
            x[rd(i)] = a | b;      // or
          else if (f7v == 0x01) {  // rem (signed; /0 gives the dividend)
            int32_t sa = (int32_t)a, sb = (int32_t)b;
            if (sb == 0)
              x[rd(i)] = a;
            else if (sa == INT32_MIN && sb == -1)
              x[rd(i)] = 0;
            else
              x[rd(i)] = (uint32_t)(sa % sb);
          } else
            illegal(fr, i);
          break;
        case 7:
          fr->rec.mnemonic = f7v == 0x00 ? "and" : f7v == 0x01 ? "remu" : NULL;
          if (f7v == 0x00)
            x[rd(i)] = a & b;    // and
          else if (f7v == 0x01)  // remu
            x[rd(i)] = b ? a % b : a;
          else
            illegal(fr, i);
          break;
      }
      break;
    }

    case 0x0f: {              // fence / fence.i / cbo.clean|inval|flush (f3 picks)
      if (f3(i) == 0) {       // fence: nothing to order, one hart
        fr->rec.mnemonic = "fence";
        break;
      }
      if (f3(i) == 1) {       // fence.i or the Zicbom cbo forms
        int funct5 = f7(i);
        if (funct5 == 0 && !rs1(i) && !rd(i)) {  // fence.i: no icache
          fr->rec.mnemonic = "fence.i";
          break;
        }
        if (funct5 > 2) illegal(fr, i);
        cbo_op(fr, i);
        break;
      }
      if (f3(i) == 2) {  // cbo.zero (Zicboz)
        if (f7(i) != 0) illegal(fr, i);
        cbo_zero(fr, i);
        break;
      }
      illegal(fr, i);
    } break;

    case 0x73: {  // system: Zicsr by f3 (4 is reserved), imm12 otherwise
      if (f3(i) == 0)
        return system_op(fr, i);
      else if (f3(i) != 4)
        csr_op(fr, i);
      else
        illegal(fr, i);
      break;
    }

    case 0x2f: {  // amo: RV32 has the word forms only (f3 2)
      if (f3(i) == 2)
        amo_op(fr, i);
      else
        illegal(fr, i);
      break;
    }

    case 0x07:  // flw / fld (f3 2/3)
      if (f3(i) == 2)
        fp_ldst(fr, i, 0, 0);
      else if (f3(i) == 3)
        fp_ldst(fr, i, 0, 1);
      else
        illegal(fr, i);
      break;

    case 0x27:  // fsw / fsd (f3 2/3)
      if (f3(i) == 2)
        fp_ldst(fr, i, 1, 0);
      else if (f3(i) == 3)
        fp_ldst(fr, i, 1, 1);
      else
        illegal(fr, i);
      break;

    case 0x43:
    case 0x47:
    case 0x4b:
    case 0x4f:  // fmadd family (rs3 field)
      fma_op(fr, i, i & 0x7f);
      break;

    case 0x53:  // fp arithmetic (f7/f3 pick; volume I chapter 8)
      fp_op(fr, i);
      break;

    default:
      illegal(fr, i);
  }
  return pc + 4;  // straight-line: the manual's pc <- pc + 4
}

// ---- compressed instructions (volume I, chapter 16) ---------------------------
// Quadrant by i[1:0]; each case is the manual's own expansion to its 32-bit
// equivalent, written directly. RV32C differs from RV64C exactly where the
// spec says so: c.jal where RV64C has c.addiw, c.flw/c.fsw where RV64C has
// c.ld/c.sd, c.flwsp/c.fswsp where RV64C has c.ldsp/c.sdsp, and the
// c.subw/c.addw encodings are reserved.

uint64_t riscv32_exec_c(frame* fr, uint16_t i16) {
  uint32_t pc = (uint32_t)rv32_cpu->pc;
  int quad = i16 & 3;
  int f3v = (i16 >> 13) & 7;

  if (quad == 0) {
    // Quadrant 0: SP-relative loads and stores on compressed regs 8..15;
    // the all-zeros encoding of c.addi4spn is reserved.
    int rdq = 8 + ((i16 >> 2) & 7);
    int rs1q = 8 + ((i16 >> 7) & 7);
    int rs2q = 8 + ((i16 >> 2) & 7);
    switch (f3v) {
      case 0: {  // c.addi4spn: rd = x[2] + nzuimm
        fr->rec.mnemonic = "c.addi4spn";
        uint32_t imm = (uint32_t)((((i16 >> 11) & 3) << 4) | (((i16 >> 7) & 15) << 6) |
                                  (((i16 >> 6) & 1) << 2) | (((i16 >> 5) & 1) << 3));
        if (imm == 0) illegal(fr, i16);  // all-zeros is reserved
        x[rdq] = x[2] + imm;
        break;
      }
      case 1: {  // c.fld
        fr->rec.mnemonic = "c.fld";
        fp_enter(fr, i16);
        uint32_t imm = (uint32_t)((((i16 >> 10) & 7) << 3) | (((i16 >> 5) & 3) << 6));
        f[rdq] = riscv32_mem_load(fr, x[rs1q] + imm, 8, acc_read);
        break;
      }
      case 2: {  // c.lw: rd = sext32(mem[x[rs1]+uimm])
        fr->rec.mnemonic = "c.lw";
        uint32_t imm = (uint32_t)((((i16 >> 10) & 7) << 3) | (((i16 >> 6) & 1) << 2) |
                                  (((i16 >> 5) & 1) << 6));
        x[rdq] = load32(fr, x[rs1q] + imm, 4, 1);
        break;
      }
      case 3: {  // c.flw (RV32C): the RV64C encoding here is c.ld
        fr->rec.mnemonic = "c.flw";
        fp_enter(fr, i16);
        uint32_t imm = (uint32_t)((((i16 >> 10) & 7) << 3) | (((i16 >> 6) & 1) << 2) |
                                  (((i16 >> 5) & 1) << 6));
        uint32_t v = (uint32_t)riscv32_mem_load(fr, x[rs1q] + imm, 4, acc_read);
        f[rdq] = 0xffffffff00000000ULL | v;
        break;
      }
      case 5: {  // c.fsd
        fr->rec.mnemonic = "c.fsd";
        fp_enter(fr, i16);
        uint32_t imm = (uint32_t)((((i16 >> 10) & 7) << 3) | (((i16 >> 5) & 3) << 6));
        riscv32_mem_store(fr, x[rs1q] + imm, 8, f[rs2q]);
        break;
      }
      case 6: {  // c.sw
        fr->rec.mnemonic = "c.sw";
        uint32_t imm = (uint32_t)((((i16 >> 10) & 7) << 3) | (((i16 >> 6) & 1) << 2) |
                                  (((i16 >> 5) & 1) << 6));
        riscv32_mem_store(fr, x[rs1q] + imm, 4, x[rs2q]);
        break;
      }
      case 7: {  // c.fsw (RV32C): the RV64C encoding here is c.sd
        fr->rec.mnemonic = "c.fsw";
        fp_enter(fr, i16);
        uint32_t imm = (uint32_t)((((i16 >> 10) & 7) << 3) | (((i16 >> 6) & 1) << 2) |
                                  (((i16 >> 5) & 1) << 6));
        riscv32_mem_store(fr, x[rs1q] + imm, 4, f[rs2q]);
        break;
      }
      default:
        illegal(fr, i16);
    }
    return pc + 2;
  }

  if (quad == 1) {
    int rdd = (i16 >> 7) & 31;
    switch (f3v) {
      case 0: {  // c.addi / c.nop
        fr->rec.mnemonic = rdd ? "c.addi" : "c.nop";
        uint32_t imm = (uint32_t)sext32((((i16 >> 12) & 1) << 5) | ((i16 >> 2) & 31), 6);
        if (rdd) x[rdd] += imm;
        break;
      }
      case 1: {  // c.jal (RV32C): the RV64C encoding here is c.addiw
        fr->rec.mnemonic = "c.jal";
        x[1] = pc + 2;
        return (uint32_t)(pc + (uint32_t)sext32(
                                     (((i16 >> 12) & 1) << 11) | (((i16 >> 11) & 1) << 4) |
                                         (((i16 >> 9) & 3) << 8) | (((i16 >> 8) & 1) << 10) |
                                         (((i16 >> 7) & 1) << 6) | (((i16 >> 6) & 1) << 7) |
                                         (((i16 >> 3) & 7) << 1) | (((i16 >> 2) & 1) << 5),
                                     12));
      }
      case 2:  // c.li
        fr->rec.mnemonic = "c.li";
        x[rdd] = (uint32_t)sext32((((i16 >> 12) & 1) << 5) | ((i16 >> 2) & 31), 6);
        break;
      case 3:
        fr->rec.mnemonic = rdd == 2 ? "c.addi16sp" : "c.lui";
        if (rdd == 2) {  // c.addi16sp
          uint32_t imm = (uint32_t)sext32(
              (((i16 >> 12) & 1) << 9) | (((i16 >> 6) & 1) << 4) | (((i16 >> 5) & 1) << 6) |
                  (((i16 >> 3) & 3) << 7) | (((i16 >> 2) & 1) << 5),
              10);
          x[2] += imm;
        } else if (rdd != 0) {  // c.lui (nzimm=0 is a hint, matches QEMU)
          uint32_t imm = (uint32_t)sext32((((i16 >> 12) & 1) << 17) | (((i16 >> 2) & 31) << 12), 18);
          if (imm) x[rdd] = imm;
        }
        break;
      case 4: {
        int sub = (i16 >> 10) & 3;
        int rdq = 8 + ((i16 >> 7) & 7);
        int rs2q = 8 + ((i16 >> 2) & 7);
        if (sub == 0 || sub == 1) {  // c.srli / c.srai
          fr->rec.mnemonic = sub == 0 ? "c.srli" : "c.srai";
          // RV32C: shamt[5] must be zero; the bit is reserved here.
          if ((i16 >> 12) & 1) illegal(fr, i16);
          int shamt = (i16 >> 2) & 31;
          if (shamt == 0) break;  // hint
          x[rdq] = sub == 0 ? x[rdq] >> shamt : (uint32_t)((int32_t)x[rdq] >> shamt);
        } else if (sub == 2) {  // c.andi
          fr->rec.mnemonic = "c.andi";
          uint32_t imm = (uint32_t)sext32((((i16 >> 12) & 1) << 5) | ((i16 >> 2) & 31), 6);
          x[rdq] &= imm;
        } else {  // c.sub / c.xor / c.or / c.and
          // RV32C: bit 12 = 1 selects the RV64C c.subw/c.addw encodings and
          // is reserved (bits 6:5 = 2/3 with bit 12 set are reserved too).
          if ((i16 >> 12) & 1) illegal(fr, i16);
          switch ((i16 >> 5) & 3) {
            case 0:
              fr->rec.mnemonic = "c.sub";
              x[rdq] -= x[rs2q];
              break;
            case 1:
              fr->rec.mnemonic = "c.xor";
              x[rdq] ^= x[rs2q];
              break;
            case 2:
              fr->rec.mnemonic = "c.or";
              x[rdq] |= x[rs2q];
              break;
            default:
              fr->rec.mnemonic = "c.and";
              x[rdq] &= x[rs2q];
              break;
          }
        }
        break;
      }
      case 5: {  // c.j: target = pc of the instruction + offset (like the
                 // branches below; the straight-line `pc + 2` exit must not
                 // run — it would slide the target two bytes)
        fr->rec.mnemonic = "c.j";
        return (uint32_t)(pc + (uint32_t)sext32(
                                     (((i16 >> 12) & 1) << 11) | (((i16 >> 11) & 1) << 4) |
                                         (((i16 >> 9) & 3) << 8) | (((i16 >> 8) & 1) << 10) |
                                         (((i16 >> 7) & 1) << 6) | (((i16 >> 6) & 1) << 7) |
                                         (((i16 >> 3) & 7) << 1) | (((i16 >> 2) & 1) << 5),
                                     12));
      }
      case 6:
      case 7: {  // c.beqz / c.bnez
        fr->rec.mnemonic = f3v == 6 ? "c.beqz" : "c.bnez";
        int rsrc = 8 + ((i16 >> 7) & 7);
        int taken = (f3v == 6) ? x[rsrc] == 0 : x[rsrc] != 0;
        if (taken)
          return (uint32_t)(pc +
                            (uint32_t)sext32((((i16 >> 12) & 1) << 8) | (((i16 >> 10) & 3) << 3) |
                                                 (((i16 >> 5) & 3) << 6) | (((i16 >> 3) & 3) << 1) |
                                                 (((i16 >> 2) & 1) << 5),
                                             9));
        break;
      }
      default:
        illegal(fr, i16);
    }
    return pc + 2;
  }

  // Quadrant 2
  int rdd = (i16 >> 7) & 31;
  int rs2v = (i16 >> 2) & 31;
  switch (f3v) {
    case 0: {  // c.slli
      fr->rec.mnemonic = "c.slli";
      // RV32C: shamt[5] must be zero; the bit is reserved here.
      if ((i16 >> 12) & 1) illegal(fr, i16);
      int shamt = (i16 >> 2) & 31;
      if (rdd == 0 || shamt == 0) break;  // hints
      x[rdd] <<= shamt;
      break;
    }
    case 1: {  // c.fldsp
      fr->rec.mnemonic = "c.fldsp";
      fp_enter(fr, i16);
      uint32_t imm =
          (uint32_t)((((i16 >> 12) & 1) << 5) | (((i16 >> 5) & 3) << 3) | (((i16 >> 2) & 7) << 6));
      if (rdd) f[rdd] = riscv32_mem_load(fr, x[2] + imm, 8, acc_read);
      break;
    }
    case 2: {  // c.lwsp (rd = 0 reserved)
      fr->rec.mnemonic = "c.lwsp";
      if (rdd == 0) illegal(fr, i16);
      uint32_t imm =
          (uint32_t)((((i16 >> 12) & 1) << 5) | (((i16 >> 4) & 7) << 2) | (((i16 >> 2) & 3) << 6));
      x[rdd] = load32(fr, x[2] + imm, 4, 1);
      break;
    }
    case 3: {  // c.flwsp (RV32C): the RV64C encoding here is c.ldsp
      fr->rec.mnemonic = "c.flwsp";
      if (rdd == 0) illegal(fr, i16);
      fp_enter(fr, i16);
      uint32_t imm =
          (uint32_t)((((i16 >> 12) & 1) << 5) | (((i16 >> 4) & 7) << 2) | (((i16 >> 2) & 3) << 6));
      uint32_t v = (uint32_t)riscv32_mem_load(fr, x[2] + imm, 4, acc_read);
      f[rdd] = 0xffffffff00000000ULL | v;
      break;
    }
    case 4:
      if (!((i16 >> 12) & 1)) {
        if (rs2v == 0) {  // c.jr (rd = 0 reserved)
          fr->rec.mnemonic = "c.jr";
          if (rdd == 0) illegal(fr, i16);
          return x[rdd] & ~1u;
        } else if (rdd != 0) {  // c.mv (rd = 0 is a hint)
          fr->rec.mnemonic = "c.mv";
          x[rdd] = x[rs2v];
        }
      } else if (rs2v == 0) {
        if (rdd == 0) {  // c.ebreak
          fr->rec.mnemonic = "c.ebreak";
          raise_(fr, kExBreakpoint, pc);
        } else {  // c.jalr: ra = pc+2, pc = x[rd] & ~1
          fr->rec.mnemonic = "c.jalr";
          uint32_t target = x[rdd] & ~1u;
          x[1] = pc + 2;
          return target;
        }
      } else if (rdd != 0) {  // c.add (rd = 0 is a hint)
        fr->rec.mnemonic = "c.add";
        x[rdd] += x[rs2v];
      }
      break;
    case 5: {  // c.fsdsp (rs2 = 0 is the reserved/hint encoding, as RV64)
      fr->rec.mnemonic = "c.fsdsp";
      fp_enter(fr, i16);
      uint32_t imm = (uint32_t)((((i16 >> 10) & 7) << 3) | (((i16 >> 7) & 7) << 6));
      if (rs2v) riscv32_mem_store(fr, x[2] + imm, 8, f[rs2v]);
      break;
    }
    case 6: {  // c.swsp
      fr->rec.mnemonic = "c.swsp";
      uint32_t imm = (uint32_t)((((i16 >> 9) & 15) << 2) | (((i16 >> 7) & 3) << 6));
      riscv32_mem_store(fr, x[2] + imm, 4, x[rs2v]);
      break;
    }
    case 7: {  // c.fswsp (RV32C): the RV64C encoding here is c.sdsp
      fr->rec.mnemonic = "c.fswsp";
      fp_enter(fr, i16);
      uint32_t imm = (uint32_t)((((i16 >> 9) & 15) << 2) | (((i16 >> 7) & 3) << 6));
      riscv32_mem_store(fr, x[2] + imm, 4, f[rs2v]);
      break;
    }
    default:
      illegal(fr, i16);
  }
  return pc + 2;  // compressed straight-line
}
