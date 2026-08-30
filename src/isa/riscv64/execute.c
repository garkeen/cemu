#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "riscv.h"
#include "isa/isa.h"
#include "host/host.h"
#include "util/log.h"

static int g_step_trace = -1;

static int StepTrace(void) {
  if (g_step_trace < 0) g_step_trace = getenv("CEMU_TRACE") != NULL;
  return g_step_trace;
}

enum { kPrivMachine = 3 };

static RiscvState *Rs(CpuState *cpu) {
  return (RiscvState *)cpu->priv;
}

static void SetGpr(RiscvState *s, int rd, uint64_t v) {
  if (rd != 0) s->gpr[rd] = v;
}

static uint64_t Sext(uint64_t v, int bits) {
  uint64_t m = 1ULL << (bits - 1);
  return ((v & ((1ULL << bits) - 1)) ^ m) - m;
}

// ---- memory access with trap checks ----

static int MemRead(CpuState *cpu, RiscvState *s, uint64_t addr, int len,
                   uint64_t *out) {
  Bus *bus = cpu->bus;
  BusRegion *r;
  if (BusProbe(bus, addr, len, &r) != 0) {
    RiscvTrap(cpu, s, kExLoadFault, addr);
    return -1;
  }
  *out = BusRead(bus, addr, len);
  return 0;
}

static int MemWrite(CpuState *cpu, RiscvState *s, uint64_t addr, int len,
                    uint64_t val) {
  Bus *bus = cpu->bus;
  BusRegion *r;
  if (BusProbe(bus, addr, len, &r) != 0) {
    RiscvTrap(cpu, s, kExStoreFault, addr);
    return -1;
  }
  BusWrite(bus, addr, len, val);
  return 0;
}

static int Illegal(CpuState *cpu, RiscvState *s, uint32_t inst) {
  RiscvTrap(cpu, s, kExIllegal, inst);
  return -1;
}

static int FpEnabled(RiscvState *s) {
  return (s->mstatus & kMstatusFs) != 0;
}

static void MarkFpDirty(RiscvState *s) {
  s->mstatus |= kMstatusFs;
}

static int ResolveRm(RiscvState *s, int rm_field) {
  return rm_field == kRmDyn ? s->frm : rm_field;
}

static uint64_t LoadValue(uint64_t v, int f3) {
  switch (f3) {
    case 0: return Sext(v & 0xffULL, 8);
    case 1: return Sext(v & 0xffffULL, 16);
    case 2: return Sext(v & 0xffffffffULL, 32);
    case 4: return v & 0xffULL;
    case 5: return v & 0xffffULL;
    case 6: return v & 0xffffffffULL;
    default: return v;
  }
}

// ---- compressed instructions ----
// Returns 0 on success with *next_pc updated, -1 when trapped.

static int ExecCompressed(CpuState *cpu, RiscvState *s, uint64_t pc,
                          uint16_t i16, uint64_t *next_pc) {
  int op = i16 & 3;
  int f3 = (i16 >> 13) & 7;

  if (op == 0) {
    int rd = 8 + ((i16 >> 2) & 7);
    int rs1 = 8 + ((i16 >> 7) & 7);
    int rs2 = 8 + ((i16 >> 2) & 7);
    switch (f3) {
      case 0: {  // c.addi4spn
        // nzuimm=0 is the all-zeros encoding: reserved, illegal
        uint64_t imm = (((i16 >> 11) & 3) << 4) | (((i16 >> 7) & 15) << 6) |
                       (((i16 >> 6) & 1) << 2) | (((i16 >> 5) & 1) << 3);
        if (rd == 0 || imm == 0) return Illegal(cpu, s, i16);
        SetGpr(s, rd, s->gpr[2] + imm);
        return 0;
      }
      case 1: {  // c.fld (rv64)
        if (!FpEnabled(s)) return Illegal(cpu, s, i16);
        MarkFpDirty(s);
        if (rs1 == 0) return Illegal(cpu, s, i16);
        uint64_t imm = (((i16 >> 10) & 7) << 3) | (((i16 >> 5) & 3) << 6);
        uint64_t v;
        if (MemRead(cpu, s, s->gpr[rs1] + imm, 8, &v) != 0) return -1;
        s->fpr[rd] = v;
        return 0;
      }
      case 2: {  // c.lw
        if (rs1 == 0) return Illegal(cpu, s, i16);
        uint64_t imm = (((i16 >> 10) & 7) << 3) | (((i16 >> 6) & 1) << 2) |
                       (((i16 >> 5) & 1) << 6);
        uint64_t v;
        if (MemRead(cpu, s, s->gpr[rs1] + imm, 4, &v) != 0) return -1;
        SetGpr(s, rd, Sext(v, 32));
        return 0;
      }
      case 3: {  // c.ld (rv64)
        if (rs1 == 0) return Illegal(cpu, s, i16);
        uint64_t imm = (((i16 >> 10) & 7) << 3) | (((i16 >> 5) & 3) << 6);
        uint64_t v;
        if (MemRead(cpu, s, s->gpr[rs1] + imm, 8, &v) != 0) return -1;
        SetGpr(s, rd, v);
        return 0;
      }
      case 5: {  // c.fsd (rv64)
        if (!FpEnabled(s)) return Illegal(cpu, s, i16);
        MarkFpDirty(s);
        if (rs1 == 0) return Illegal(cpu, s, i16);
        uint64_t imm = (((i16 >> 10) & 7) << 3) | (((i16 >> 5) & 3) << 6);
        return MemWrite(cpu, s, s->gpr[rs1] + imm, 8, s->fpr[rs2]);
      }
      case 6: {  // c.sw
        if (rs1 == 0) return Illegal(cpu, s, i16);
        uint64_t imm = (((i16 >> 10) & 7) << 3) | (((i16 >> 6) & 1) << 2) |
                       (((i16 >> 5) & 1) << 6);
        return MemWrite(cpu, s, s->gpr[rs1] + imm, 4, s->gpr[rs2]);
      }
      case 7: {  // c.sd (rv64)
        if (rs1 == 0) return Illegal(cpu, s, i16);
        uint64_t imm = (((i16 >> 10) & 7) << 3) | (((i16 >> 5) & 3) << 6);
        return MemWrite(cpu, s, s->gpr[rs1] + imm, 8, s->gpr[rs2]);
      }
      default:
        return Illegal(cpu, s, i16);
    }
  }

  if (op == 1) {
    int rd = (i16 >> 7) & 31;
    switch (f3) {
      case 0: {  // c.addi / c.nop
        uint64_t imm = Sext((((i16 >> 12) & 1) << 5) | ((i16 >> 2) & 31), 6);
        if (rd != 0) SetGpr(s, rd, s->gpr[rd] + imm);
        return 0;
      }
      case 1: {  // c.addiw (rv64; c.jal is rv32-only)
        if (rd == 0) return Illegal(cpu, s, i16);
        uint64_t imm = Sext((((i16 >> 12) & 1) << 5) | ((i16 >> 2) & 31), 6);
        SetGpr(s, rd, Sext((uint32_t)(s->gpr[rd] + imm), 32));
        return 0;
      }
      case 2: {  // c.li
        uint64_t imm = Sext((((i16 >> 12) & 1) << 5) | ((i16 >> 2) & 31), 6);
        SetGpr(s, rd, imm);
        return 0;
      }
      case 3: {
        if (rd == 2) {  // c.addi16sp
          uint64_t nzimm = (((i16 >> 12) & 1) << 9) | (((i16 >> 6) & 1) << 4) |
                           (((i16 >> 5) & 1) << 6) | (((i16 >> 3) & 3) << 7) |
                           (((i16 >> 2) & 1) << 5);
          SetGpr(s, 2, s->gpr[2] + Sext(nzimm, 10));
          return 0;
        }
        if (rd == 0) return 0;  // hint
        uint64_t imm =
            Sext((((i16 >> 12) & 1) << 17) | (((i16 >> 2) & 31) << 12), 18);
        if (imm == 0) return 0;  // hint
        SetGpr(s, rd, imm);
        return 0;
      }
      case 4: {
        int sub_op = (i16 >> 10) & 3;
        int rdq = 8 + ((i16 >> 7) & 7);
        int rs2q = 8 + ((i16 >> 2) & 7);
        if (sub_op == 0 || sub_op == 1) {  // c.srli / c.srai
          if (rdq == 0) return 0;  // hint
          int shamt = (((i16 >> 12) & 1) << 5) | ((i16 >> 2) & 31);
          if (shamt == 0) return 0;  // hint
          if (sub_op == 0) {
            SetGpr(s, rdq, s->gpr[rdq] >> shamt);
          } else {
            SetGpr(s, rdq, (uint64_t)((int64_t)s->gpr[rdq] >> shamt));
          }
          return 0;
        }
        if (sub_op == 2) {  // c.andi
          if (rdq == 0) return 0;
          uint64_t imm = Sext((((i16 >> 12) & 1) << 5) | ((i16 >> 2) & 31), 6);
          SetGpr(s, rdq, s->gpr[rdq] & imm);
          return 0;
        }
        if (rdq == 0 || rs2q == 0) return 0;  // hints
        if ((i16 >> 12) & 1) {
          // c.subw (00) / c.addw (01) for rv64
          switch ((i16 >> 5) & 3) {
            case 0:
              SetGpr(s, rdq, Sext((uint32_t)(s->gpr[rdq] - s->gpr[rs2q]), 32));
              return 0;
            case 1:
              SetGpr(s, rdq, Sext((uint32_t)(s->gpr[rdq] + s->gpr[rs2q]), 32));
              return 0;
            default:
              return Illegal(cpu, s, i16);
          }
        }
        switch ((i16 >> 5) & 3) {
          case 0: SetGpr(s, rdq, s->gpr[rdq] - s->gpr[rs2q]); break;  // c.sub
          case 1: SetGpr(s, rdq, s->gpr[rdq] ^ s->gpr[rs2q]); break;  // c.xor
          case 2: SetGpr(s, rdq, s->gpr[rdq] | s->gpr[rs2q]); break;  // c.or
          case 3: SetGpr(s, rdq, s->gpr[rdq] & s->gpr[rs2q]); break;  // c.and
        }
        return 0;
      }
      case 5: {  // c.j
        int64_t off =
            Sext((((i16 >> 12) & 1) << 11) | (((i16 >> 11) & 1) << 4) |
                     (((i16 >> 9) & 3) << 8) | (((i16 >> 8) & 1) << 10) |
                     (((i16 >> 7) & 1) << 6) | (((i16 >> 6) & 1) << 7) |
                     (((i16 >> 3) & 7) << 1) | (((i16 >> 2) & 1) << 5),
                 12);
        *next_pc = (uint64_t)((int64_t)pc + off);
        return 0;
      }
      case 6:   // c.beqz
      case 7: {  // c.bnez
        int64_t off =
            Sext((((i16 >> 12) & 1) << 8) | (((i16 >> 10) & 3) << 3) |
                     (((i16 >> 5) & 3) << 6) | (((i16 >> 3) & 3) << 1) |
                     (((i16 >> 2) & 1) << 5),
                 9);
        int rdq = 8 + ((i16 >> 7) & 7);
        int take = (f3 == 6) ? (s->gpr[rdq] == 0) : (s->gpr[rdq] != 0);
        if (take) *next_pc = (uint64_t)((int64_t)pc + off);
        return 0;
      }
      default:
        return Illegal(cpu, s, i16);
    }
  }

  // op == 2
  int rd = (i16 >> 7) & 31;
  int rs2 = (i16 >> 2) & 31;
  switch (f3) {
    case 0: {  // c.slli
      if (rd == 0) return 0;
      int shamt = (((i16 >> 12) & 1) << 5) | ((i16 >> 2) & 31);
      if (shamt == 0) return 0;
      SetGpr(s, rd, s->gpr[rd] << shamt);
      return 0;
    }
    case 1: {  // c.fldsp (rv64)
      if (!FpEnabled(s)) return Illegal(cpu, s, i16);
      MarkFpDirty(s);
      if (rd == 0) return 0;
      uint64_t imm = (((i16 >> 12) & 1) << 5) | (((i16 >> 5) & 3) << 3) |
                     (((i16 >> 2) & 7) << 6);
      uint64_t v;
      if (MemRead(cpu, s, s->gpr[2] + imm, 8, &v) != 0) return -1;
      s->fpr[rd] = v;
      return 0;
    }
    case 2: {  // c.lwsp
      if (rd == 0) return Illegal(cpu, s, i16);
      uint64_t imm = (((i16 >> 12) & 1) << 5) | (((i16 >> 4) & 7) << 2) |
                     (((i16 >> 2) & 3) << 6);
      uint64_t v;
      if (MemRead(cpu, s, s->gpr[2] + imm, 4, &v) != 0) return -1;
      SetGpr(s, rd, Sext(v, 32));
      return 0;
    }
    case 3: {  // c.ldsp (rv64)
      if (rd == 0) return Illegal(cpu, s, i16);
      uint64_t imm = (((i16 >> 12) & 1) << 5) | (((i16 >> 5) & 3) << 3) |
                     (((i16 >> 2) & 7) << 6);
      uint64_t v;
      if (MemRead(cpu, s, s->gpr[2] + imm, 8, &v) != 0) return -1;
      SetGpr(s, rd, v);
      return 0;
    }
    case 4: {
      if (!((i16 >> 12) & 1)) {
        if (rs2 == 0) {  // c.jr
          if (rd == 0) return Illegal(cpu, s, i16);
          *next_pc = s->gpr[rd] & ~1ULL;
          return 0;
        }
        if (rd == 0) return 0;  // c.mv hint
        SetGpr(s, rd, s->gpr[rs2]);
        return 0;
      }
      if (rs2 == 0) {
        if (rd == 0) {  // c.ebreak
          RiscvTrap(cpu, s, kExBreakpoint, pc);
          return -1;
        }
        // c.jalr rs1: ra <- pc+2, pc <- rs1 & ~1
        uint64_t target = s->gpr[rd] & ~1ULL;
        SetGpr(s, 1, pc + 2);
        *next_pc = target;
        return 0;
      }
      if (rd == 0) return 0;  // c.add hint
      SetGpr(s, rd, s->gpr[rd] + s->gpr[rs2]);
      return 0;
    }
    case 5: {  // c.fsdsp (rv64)
      if (!FpEnabled(s)) return Illegal(cpu, s, i16);
      MarkFpDirty(s);
      if (rs2 == 0) return 0;
      uint64_t imm = (((i16 >> 10) & 7) << 3) | (((i16 >> 7) & 7) << 6);
      return MemWrite(cpu, s, s->gpr[2] + imm, 8, s->fpr[rs2]);
    }
    case 6: {  // c.swsp
      uint64_t imm = (((i16 >> 9) & 15) << 2) | (((i16 >> 7) & 3) << 6);
      return MemWrite(cpu, s, s->gpr[2] + imm, 4, s->gpr[rs2]);
    }
    case 7: {  // c.sdsp (rv64)
      uint64_t imm = (((i16 >> 10) & 7) << 3) | (((i16 >> 7) & 7) << 6);
      return MemWrite(cpu, s, s->gpr[2] + imm, 8, s->gpr[rs2]);
    }
    default:
      return Illegal(cpu, s, i16);
  }
}

// ---- integer multiply/divide ----

static int MulDiv(CpuState *cpu, RiscvState *s, uint32_t inst, int f3,
                  uint64_t a, uint64_t b, uint64_t *out) {
  int64_t sa = (int64_t)a, sb = (int64_t)b;
  switch (f3) {
    case 0: *out = a * b; return 0;
    case 1: *out = (uint64_t)(((__int128)sa * sb) >> 64); return 0;
    case 2:
      *out = (uint64_t)(((__int128)sa * (unsigned __int128)b) >> 64);
      return 0;
    case 3: *out = (uint64_t)((((unsigned __int128)a) * b) >> 64); return 0;
    case 4:
      if (sb == 0) *out = (uint64_t)-1;
      else if (sa == INT64_MIN && sb == -1) *out = (uint64_t)INT64_MIN;
      else *out = (uint64_t)(sa / sb);
      return 0;
    case 5: *out = b == 0 ? (uint64_t)-1 : a / b; return 0;
    case 6:
      if (sb == 0) *out = a;
      else if (sa == INT64_MIN && sb == -1) *out = 0;
      else *out = (uint64_t)(sa % sb);
      return 0;
    case 7: *out = b == 0 ? a : a % b; return 0;
    default: return Illegal(cpu, s, inst);
  }
}

static int MulDiv32(CpuState *cpu, RiscvState *s, uint32_t inst, int f3,
                    uint64_t a, uint64_t b, uint64_t *out) {
  int32_t sa = (int32_t)a, sb = (int32_t)b;
  int32_t r;
  switch (f3) {
    case 0: r = (int32_t)((uint32_t)a * (uint32_t)b); break;
    case 4:
      if (sb == 0) r = -1;
      else if (sa == INT32_MIN && sb == -1) r = INT32_MIN;
      else r = (int32_t)(sa / sb);
      break;
    case 5: r = sb == 0 ? -1 : (int32_t)((uint32_t)a / (uint32_t)b); break;
    case 6:
      if (sb == 0) r = sa;
      else if (sa == INT32_MIN && sb == -1) r = 0;
      else r = (int32_t)(sa % sb);
      break;
    case 7: r = sb == 0 ? sa : (int32_t)((uint32_t)a % (uint32_t)b); break;
    default: return Illegal(cpu, s, inst);
  }
  *out = Sext((uint32_t)r, 32);
  return 0;
}

// ---- atomics ----

static int ExecAtomic(CpuState *cpu, RiscvState *s, uint32_t inst,
                      int is_double) {
  int f5 = (int)(inst >> 27);
  int rs1 = (int)((inst >> 15) & 31);
  int rs2 = (int)((inst >> 20) & 31);
  int rd = (int)((inst >> 7) & 31);
  int width = is_double ? 8 : 4;
  uint64_t addr = s->gpr[rs1];
  if (addr % (uint64_t)width) {
    RiscvTrap(cpu, s, kExStoreMisaligned, addr);
    return -1;
  }
  Bus *bus = cpu->bus;
  BusRegion *r;
  if (BusProbe(bus, addr, width, &r) != 0) {
    RiscvTrap(cpu, s, kExLoadFault, addr);
    return -1;
  }
  uint64_t old, val, stored;
  switch (f5) {
    case 0x02:  // lr
      if (MemRead(cpu, s, addr, width, &old) != 0) return -1;
      s->res_valid = 1;
      s->res_addr = addr;
      SetGpr(s, rd, is_double ? old : Sext(old, 32));
      return 0;
    case 0x03:  // sc
      val = s->gpr[rs2];
      if (s->res_valid && s->res_addr == addr) {
        if (MemWrite(cpu, s, addr, width, val) != 0) return -1;
        SetGpr(s, rd, 0);
      } else {
        SetGpr(s, rd, 1);
      }
      s->res_valid = 0;
      return 0;
    case 0x01: case 0x00: case 0x04: case 0x0C:
    case 0x08: case 0x10: case 0x14: case 0x18: case 0x1C: {
      if (MemRead(cpu, s, addr, width, &old) != 0) return -1;
      val = s->gpr[rs2];
      if (is_double) {
        switch (f5) {
          case 0x01: stored = val; break;                                 // amoswap
          case 0x00: stored = old + val; break;                           // amoadd
          case 0x04: stored = old ^ val; break;                           // amoxor
          case 0x0C: stored = old & val; break;                           // amoand
          case 0x08: stored = old | val; break;                           // amoor
          case 0x10: stored = (int64_t)old < (int64_t)val ? old : val; break;
          case 0x14: stored = (int64_t)old > (int64_t)val ? old : val; break;
          case 0x18: stored = old < val ? old : val; break;
          default:   stored = old > val ? old : val; break;               // amomaxu
        }
      } else {
        uint32_t o32 = (uint32_t)old, v32 = (uint32_t)val;
        int32_t so = (int32_t)o32, sv = (int32_t)v32;
        switch (f5) {
          case 0x01: stored = v32; break;
          case 0x00: stored = o32 + v32; break;
          case 0x04: stored = o32 ^ v32; break;
          case 0x0C: stored = o32 & v32; break;
          case 0x08: stored = o32 | v32; break;
          case 0x10: stored = so < sv ? o32 : v32; break;
          case 0x14: stored = so > sv ? o32 : v32; break;
          case 0x18: stored = o32 < v32 ? o32 : v32; break;
          default:   stored = o32 > v32 ? o32 : v32; break;
        }
      }
      if (!is_double) stored &= 0xffffffffULL;
      if (MemWrite(cpu, s, addr, width, stored) != 0) return -1;
      SetGpr(s, rd, is_double ? old : Sext(old, 32));
      return 0;
    }
    default:
      return Illegal(cpu, s, inst);
  }
}

// ---- integer register executors ----

static int ExecOpImm(CpuState *cpu, RiscvState *s, uint32_t inst) {
  int f3 = (int)((inst >> 12) & 7);
  int rd = (int)((inst >> 7) & 31);
  int rs1 = (int)((inst >> 15) & 31);
  uint64_t imm = Sext(inst >> 20, 12);
  uint64_t a = s->gpr[rs1];
  uint64_t r = 0;
  switch (f3) {
    case 0: r = a + imm; break;
    case 1:
      if ((inst >> 26) != 0) return Illegal(cpu, s, inst);
      r = a << ((inst >> 20) & 63);
      break;
    case 2: r = (int64_t)a < (int64_t)imm ? 1 : 0; break;
    case 3: r = a < imm ? 1 : 0; break;
    case 4: r = a ^ imm; break;
    case 5: {
      uint64_t f6 = inst >> 26;
      if (f6 == 0) r = a >> ((inst >> 20) & 63);
      else if (f6 == 0x10) r = (uint64_t)((int64_t)a >> ((inst >> 20) & 63));
      else return Illegal(cpu, s, inst);
      break;
    }
    case 6: r = a | imm; break;
    case 7: r = a & imm; break;
    default: return Illegal(cpu, s, inst);
  }
  SetGpr(s, rd, r);
  return 0;
}

static int ExecOpImm32(CpuState *cpu, RiscvState *s, uint32_t inst) {
  int f3 = (int)((inst >> 12) & 7);
  int rd = (int)((inst >> 7) & 31);
  int rs1 = (int)((inst >> 15) & 31);
  uint64_t imm = Sext(inst >> 20, 12);
  uint64_t a = s->gpr[rs1];
  uint64_t r;
  switch (f3) {
    case 0: r = Sext((uint32_t)(a + imm), 32); break;
    case 1:
      if ((inst >> 25) != 0) return Illegal(cpu, s, inst);
      r = Sext((uint32_t)(a << (imm & 31)), 32);
      break;
    case 5: {
      uint64_t f7 = inst >> 25;
      if (f7 == 0) r = Sext((uint32_t)a >> (imm & 31), 32);
      else if (f7 == 0x20) r = Sext((uint32_t)((int32_t)a >> (imm & 31)), 32);
      else return Illegal(cpu, s, inst);
      break;
    }
    default: return Illegal(cpu, s, inst);
  }
  SetGpr(s, rd, r);
  return 0;
}

static int ExecOp(CpuState *cpu, RiscvState *s, uint32_t inst) {
  int f3 = (int)((inst >> 12) & 7);
  int f7 = (int)(inst >> 25);
  int rd = (int)((inst >> 7) & 31);
  int rs1 = (int)((inst >> 15) & 31);
  int rs2 = (int)((inst >> 20) & 31);
  uint64_t a = s->gpr[rs1], b = s->gpr[rs2];
  uint64_t r = 0;
  if (f7 == 1) {
    int rc = MulDiv(cpu, s, inst, f3, a, b, &r);
    if (rc) return rc;
    SetGpr(s, rd, r);
    return 0;
  }
  switch (f7) {
    case 0x00:
      switch (f3) {
        case 0: r = a + b; break;
        case 1: r = a << (b & 63); break;
        case 2: r = (int64_t)a < (int64_t)b ? 1 : 0; break;
        case 3: r = a < b ? 1 : 0; break;
        case 4: r = a ^ b; break;
        case 5: r = a >> (b & 63); break;
        case 6: r = a | b; break;
        case 7: r = a & b; break;
      }
      break;
    case 0x20:
      switch (f3) {
        case 0: r = a - b; break;
        case 5: r = (uint64_t)((int64_t)a >> (b & 63)); break;
        default: return Illegal(cpu, s, inst);
      }
      break;
    default: return Illegal(cpu, s, inst);
  }
  SetGpr(s, rd, r);
  return 0;
}

static int ExecOp32(CpuState *cpu, RiscvState *s, uint32_t inst) {
  int f3 = (int)((inst >> 12) & 7);
  int f7 = (int)(inst >> 25);
  int rd = (int)((inst >> 7) & 31);
  int rs1 = (int)((inst >> 15) & 31);
  int rs2 = (int)((inst >> 20) & 31);
  uint64_t a = s->gpr[rs1], b = s->gpr[rs2];
  uint64_t r = 0;
  if (f7 == 1) {
    int rc = MulDiv32(cpu, s, inst, f3, a, b, &r);
    if (rc) return rc;
    SetGpr(s, rd, r);
    return 0;
  }
  switch (f7) {
    case 0x00:
      switch (f3) {
        case 0: r = Sext((uint32_t)(a + b), 32); break;
        case 1: r = Sext((uint32_t)(a << (b & 31)), 32); break;
        case 5: r = Sext((uint32_t)a >> (b & 31), 32); break;
        default: return Illegal(cpu, s, inst);
      }
      break;
    case 0x20:
      switch (f3) {
        case 0: r = Sext((uint32_t)(a - b), 32); break;
        case 5: r = Sext((uint32_t)((int32_t)a >> (b & 31)), 32); break;
        default: return Illegal(cpu, s, inst);
      }
      break;
    default: return Illegal(cpu, s, inst);
  }
  SetGpr(s, rd, r);
  return 0;
}

static int ExecSystem(CpuState *cpu, RiscvState *s, uint32_t inst,
                      uint64_t *next_pc) {
  int f3 = (int)((inst >> 12) & 7);
  uint32_t imm12 = (inst >> 20) & 0xfff;
  if (f3 == 0) {
    int rd = (int)((inst >> 7) & 31);
    int rs1 = (int)((inst >> 15) & 31);
    switch (imm12) {
      case 0x000:  // ecall
        RiscvTrap(cpu, s, s->priv == kPrivMachine ? kExMachineEcall
                                                  : kExUserEcall, 0);
        return -1;
      case 0x001:  // ebreak
        RiscvTrap(cpu, s, kExBreakpoint, 0);
        return -1;
      case 0x302:  // mret
        if (rd != 0 || rs1 != 0) return Illegal(cpu, s, inst);
        *next_pc = RiscvMret(s);
        return 0;
      case 0x105:  // wfi
        return 0;
      case 0x102:  // sret: S mode not implemented
      default:
        return Illegal(cpu, s, inst);
    }
  }
  if (f3 == 4) return Illegal(cpu, s, inst);

  // Zicsr
  int rd = (int)((inst >> 7) & 31);
  int rs1 = (int)((inst >> 15) & 31);
  uint64_t csr = inst >> 20;
  uint64_t old;
  if (RiscvCsrRead(s, csr, &old) != 0) return Illegal(cpu, s, inst);
  uint64_t src = (f3 >= 5) ? (uint64_t)(rs1 & 31) : s->gpr[rs1];
  int do_write = 1;
  uint64_t wval = old;
  switch (f3 & 3) {
    case 1:
      wval = src;
      break;
    case 2:  // csrrs / csrrsi
      do_write = (f3 < 5) ? (rs1 != 0) : ((rs1 & 31) != 0);
      wval = old | src;
      break;
    case 3:  // csrrc / csrrci
      do_write = (f3 < 5) ? (rs1 != 0) : ((rs1 & 31) != 0);
      wval = old & ~src;
      break;
    default:
      return Illegal(cpu, s, inst);
  }
  if (do_write && RiscvCsrWrite(s, csr, wval, *next_pc) != 0)
    return Illegal(cpu, s, inst);
  SetGpr(s, rd, old);
  return 0;
}

// ---- float arithmetic (f7 selects op, fmt bit0 selects S/D) ----

static int ExecFloat(CpuState *cpu, RiscvState *s, uint32_t inst,
                     int is_double) {
  int f3 = (int)((inst >> 12) & 7);
  int rs1 = (int)((inst >> 15) & 31);
  int rs2 = (int)((inst >> 20) & 31);
  int rd = (int)((inst >> 7) & 31);
  int f7 = (int)(inst >> 25);
  uint64_t a = s->fpr[rs1], b = s->fpr[rs2];
  uint64_t res;

  if (f7 == 0x10 || f7 == 0x11) {  // fsgnj (f3=0) / fsgnjn (1) / fsgnjx (2)
    if (f3 > 2) return Illegal(cpu, s, inst);
    res = is_double ? RiscvFpSgnjD(a, b, f3) : RiscvFpSgnjS(a, b, f3);
  } else if (f7 == 0x14 || f7 == 0x15) {  // fmin (f3=0) / fmax (1)
    if (f3 > 1) return Illegal(cpu, s, inst);
    res = is_double ? RiscvFpMinMaxD(s, a, b, f3 == 1)
                    : RiscvFpMinMaxS(s, a, b, f3 == 1);
  } else if (f7 == 0x2C || f7 == 0x2D) {  // fsqrt
    if (rs2 != 0) return Illegal(cpu, s, inst);
    int rm = ResolveRm(s, f3);
    res = is_double ? RiscvFpSqrtD(s, a, rm) : RiscvFpSqrtS(s, a, rm);
  } else if (f7 <= 0x0D && (f7 & 3) == is_double) {  // add/sub/mul/div, f3=rm
    int kind = f7 >> 2;
    int rm = ResolveRm(s, f3);
    res = is_double ? RiscvFpBinaryD(s, a, b, kind, rm)
                    : RiscvFpBinaryS(s, a, b, kind, rm);
  } else {
    return Illegal(cpu, s, inst);
  }
  s->fpr[rd] = res;
  return 0;
}

static int ExecFma(RiscvState *s, uint32_t inst, int is_double,
                   int negate_product, int negate_addend) {
  int rm = ResolveRm(s, (int)((inst >> 12) & 7));
  int rs1 = (int)((inst >> 15) & 31);
  int rs2 = (int)((inst >> 20) & 31);
  int rs3 = (int)(inst >> 27);
  int rd = (int)((inst >> 7) & 31);
  uint64_t a = s->fpr[rs1], c = s->fpr[rs3];
  uint64_t r;
  if (is_double) {
    if (negate_product) a ^= 0x8000000000000000ULL;
    if (negate_addend) c ^= 0x8000000000000000ULL;
    r = RiscvFpFmaD(s, a, s->fpr[rs2], c, rm);
  } else {
    if (negate_product) a ^= 0x0000000080000000ULL;
    if (negate_addend) c ^= 0x0000000080000000ULL;
    r = RiscvFpFmaS(s, a, s->fpr[rs2], c, rm);
  }
  s->fpr[rd] = r;
  return 0;
}

static int ExecFpConvert(CpuState *cpu, RiscvState *s, uint32_t inst) {
  int rmf = (int)((inst >> 12) & 7);
  int f3 = (int)((inst >> 12) & 7);
  int rs1 = (int)((inst >> 15) & 31);
  int rs2 = (int)((inst >> 20) & 31);
  int rd = (int)((inst >> 7) & 31);
  int f7 = (int)(inst >> 25);
  int rm = ResolveRm(s, rmf);
  uint64_t res;

  if (f7 == 0x50 || f7 == 0x51) {  // fle (f3=0) / flt (1) / feq (2)
    int kind = (f3 == 0) ? kFpLe : (f3 == 1) ? kFpLt : kFpEq;
    res = (f7 == 0x51) ? RiscvFpCmpD(s, s->fpr[rs1], s->fpr[rs2], kind)
                       : RiscvFpCmpS(s, s->fpr[rs1], s->fpr[rs2], kind);
    SetGpr(s, rd, res);
    return 0;
  }

  switch (f7) {
    case 0x60:  // F: fcvt.w.s / wu.s / l.s / lu.s by rs2
      switch (rs2) {
        case 0: res = RiscvFpF2IS(s, s->fpr[rs1], 1, 32, rm); break;
        case 1: res = RiscvFpF2IS(s, s->fpr[rs1], 0, 32, rm); break;
        case 2: res = RiscvFpF2IS(s, s->fpr[rs1], 1, 64, rm); break;
        case 3: res = RiscvFpF2IS(s, s->fpr[rs1], 0, 64, rm); break;
        default: return Illegal(cpu, s, inst);
      }
      SetGpr(s, rd, res);
      return 0;
    case 0x61:  // D: fcvt.w.d / wu.d / l.d / lu.d
      switch (rs2) {
        case 0: res = RiscvFpF2ID(s, s->fpr[rs1], 1, 32, rm); break;
        case 1: res = RiscvFpF2ID(s, s->fpr[rs1], 0, 32, rm); break;
        case 2: res = RiscvFpF2ID(s, s->fpr[rs1], 1, 64, rm); break;
        case 3: res = RiscvFpF2ID(s, s->fpr[rs1], 0, 64, rm); break;
        default: return Illegal(cpu, s, inst);
      }
      SetGpr(s, rd, res);
      return 0;
    case 0x68:  // F: fcvt.s.w / s.wu / s.l / s.lu (rs2 picks width)
      res = RiscvFpI2FS(s, s->gpr[rs1], (rs2 & 1) == 0, (rs2 >= 2) ? 64 : 32,
                        rm);
      s->fpr[rd] = res;
      return 0;
    case 0x69:  // D: fcvt.d.w / d.wu / d.l / d.lu
      res = RiscvFpI2FD(s, s->gpr[rs1], (rs2 & 1) == 0, (rs2 >= 2) ? 64 : 32,
                        rm);
      s->fpr[rd] = res;
      return 0;
    case 0x20:  // fcvt.s.d (rs2 = 1, f3 = rounding mode)
      if (rs2 != 1) return Illegal(cpu, s, inst);
      s->fpr[rd] = RiscvFpCvtSD(s, s->fpr[rs1], rm);
      return 0;
    case 0x21:  // fcvt.d.s (rs2 = 0)
      if (rs2 != 0) return Illegal(cpu, s, inst);
      s->fpr[rd] = RiscvFpCvtDS(s, s->fpr[rs1], rm);
      return 0;
    case 0x70:  // F: fmv.x.w (f3=0, sign-extends per rv64) / fclass.s (f3=1)
      if (rs2 != 0) return Illegal(cpu, s, inst);
      if (f3 == 0) SetGpr(s, rd, Sext(s->fpr[rs1] & 0xffffffffULL, 32));
      else if (f3 == 1) SetGpr(s, rd, RiscvFpClassS(s->fpr[rs1]));
      else return Illegal(cpu, s, inst);
      return 0;
    case 0x71:  // D: fmv.x.d (f3=0) / fclass.d (f3=1)
      if (rs2 != 0) return Illegal(cpu, s, inst);
      if (f3 == 0) SetGpr(s, rd, s->fpr[rs1]);
      else if (f3 == 1) SetGpr(s, rd, RiscvFpClassD(s->fpr[rs1]));
      else return Illegal(cpu, s, inst);
      return 0;
    case 0x78:  // fmv.w.x
      if (rs2 != 0 || f3 != 0) return Illegal(cpu, s, inst);
      s->fpr[rd] = 0xffffffff00000000ULL | (s->gpr[rs1] & 0xffffffffULL);
      return 0;
    case 0x79:  // fmv.d.x
      if (rs2 != 0 || f3 != 0) return Illegal(cpu, s, inst);
      s->fpr[rd] = s->gpr[rs1];
      return 0;
    default:
      return Illegal(cpu, s, inst);
  }
}

static int Exec32(CpuState *cpu, RiscvState *s, uint64_t pc, uint32_t inst,
                  uint64_t *next_pc) {
  int op = (int)(inst & 0x7f);
  int rd = (int)((inst >> 7) & 31);
  int f3 = (int)((inst >> 12) & 7);
  int rs1 = (int)((inst >> 15) & 31);
  int rs2 = (int)((inst >> 20) & 31);
  uint64_t imm_i = Sext(inst >> 20, 12);
  uint64_t imm_s = Sext(((inst >> 25) << 5) | ((inst >> 7) & 31), 12);
  uint64_t imm_b = Sext((((inst >> 31) & 1) << 12) | (((inst >> 7) & 1) << 11) |
                            (((inst >> 25) & 0x3f) << 5) |
                            (((inst >> 8) & 15) << 1), 13);
  uint64_t imm_u = Sext(inst & 0xfffff000ULL, 32);
  uint64_t imm_j = Sext((((inst >> 31) & 1) << 20) |
                            (((inst >> 12) & 0xff) << 12) |
                            (((inst >> 20) & 1) << 11) |
                            (((inst >> 21) & 0x3ff) << 1), 21);
  uint64_t a = s->gpr[rs1], b = s->gpr[rs2];

  switch (op) {
    case 0x37:  // lui
      SetGpr(s, rd, imm_u);
      return 0;
    case 0x17:  // auipc
      SetGpr(s, rd, pc + imm_u);
      return 0;
    case 0x6F:  // jal
      SetGpr(s, rd, pc + 4);
      *next_pc = pc + imm_j;
      return 0;
    case 0x67:  // jalr
      if (f3 != 0) return Illegal(cpu, s, inst);
      SetGpr(s, rd, pc + 4);
      *next_pc = (a + imm_i) & ~1ULL;
      return 0;
    case 0x63: {  // branches
      int take = 0;
      switch (f3) {
        case 0: take = a == b; break;
        case 1: take = a != b; break;
        case 4: take = (int64_t)a < (int64_t)b; break;
        case 5: take = (int64_t)a >= (int64_t)b; break;
        case 6: take = a < b; break;
        case 7: take = a >= b; break;
        default: return Illegal(cpu, s, inst);
      }
      if (take) *next_pc = pc + imm_b;
      return 0;
    }
    case 0x03: {  // loads
      uint64_t addr = a + imm_i, v;
      int len;
      switch (f3) {
        case 0: case 4: len = 1; break;
        case 1: case 5: len = 2; break;
        case 2: case 6: len = 4; break;
        case 3: len = 8; break;
        default: return Illegal(cpu, s, inst);
      }
      if (MemRead(cpu, s, addr, len, &v) != 0) return -1;
      SetGpr(s, rd, LoadValue(v, f3));
      return 0;
    }
    case 0x23: {  // stores
      uint64_t addr = a + imm_s;
      switch (f3) {
        case 0: return MemWrite(cpu, s, addr, 1, b);
        case 1: return MemWrite(cpu, s, addr, 2, b);
        case 2: return MemWrite(cpu, s, addr, 4, b);
        case 3: return MemWrite(cpu, s, addr, 8, b);
        default: return Illegal(cpu, s, inst);
      }
    }
    case 0x13:
      return ExecOpImm(cpu, s, inst);
    case 0x1B:
      return ExecOpImm32(cpu, s, inst);
    case 0x33:
      return ExecOp(cpu, s, inst);
    case 0x3B:
      return ExecOp32(cpu, s, inst);
    case 0x0F:  // fence / fence.i
      if (f3 == 0 || f3 == 1) return 0;
      return Illegal(cpu, s, inst);
    case 0x73:
      return ExecSystem(cpu, s, inst, next_pc);
    case 0x2F:  // atomics
      if (f3 == 2) return ExecAtomic(cpu, s, inst, 0);
      if (f3 == 3) return ExecAtomic(cpu, s, inst, 1);
      return Illegal(cpu, s, inst);
    case 0x07: {  // flw / fld
      if (f3 != 2 && f3 != 3) return Illegal(cpu, s, inst);
      if (!FpEnabled(s)) return Illegal(cpu, s, inst);
      MarkFpDirty(s);
      int len = (f3 == 3) ? 8 : 4;
      uint64_t v;
      if (MemRead(cpu, s, a + imm_i, len, &v) != 0) return -1;
      s->fpr[rd] = (len == 4) ? (0xffffffff00000000ULL | v) : v;
      return 0;
    }
    case 0x27: {  // fsw / fsd
      if (f3 != 2 && f3 != 3) return Illegal(cpu, s, inst);
      if (!FpEnabled(s)) return Illegal(cpu, s, inst);
      MarkFpDirty(s);
      int len = (f3 == 3) ? 8 : 4;
      return MemWrite(cpu, s, a + imm_s, len, s->fpr[rs2]);
    }
    case 0x43: case 0x47: case 0x4B: case 0x4F: {  // FMAs
      if (!FpEnabled(s)) return Illegal(cpu, s, inst);
      MarkFpDirty(s);
      int is_double = (int)((inst >> 25) & 1);
      int neg_prod = (op == 0x4B || op == 0x4F);
      int neg_add = (op == 0x47 || op == 0x4F);
      return ExecFma(s, inst, is_double, neg_prod, neg_add);
    }
    case 0x53: {  // float ops and conversions
      int f7 = (int)(inst >> 25);
      switch (f7) {
        case 0x00: case 0x04: case 0x08: case 0x0C: case 0x2C:
        case 0x10: case 0x14:  // single arithmetic
          if (!FpEnabled(s)) return Illegal(cpu, s, inst);
          MarkFpDirty(s);
          return ExecFloat(cpu, s, inst, 0);
        case 0x01: case 0x05: case 0x09: case 0x0D: case 0x2D:
        case 0x11: case 0x15:  // double arithmetic
          if (!FpEnabled(s)) return Illegal(cpu, s, inst);
          MarkFpDirty(s);
          return ExecFloat(cpu, s, inst, 1);
        case 0x60: case 0x61: case 0x68: case 0x69:
        case 0x20: case 0x21:
        case 0x50: case 0x51:
        case 0x70: case 0x71: case 0x78: case 0x79:  // conversions/moves/cmp
          if (!FpEnabled(s)) return Illegal(cpu, s, inst);
          MarkFpDirty(s);
          return ExecFpConvert(cpu, s, inst);
        default:
          return Illegal(cpu, s, inst);
      }
    }
    default:
      return Illegal(cpu, s, inst);
  }
}

void RiscvStep(CpuState *cpu) {
  RiscvState *s = Rs(cpu);
  RiscvDeliverPendingInterrupt(cpu, s);
  if (cpu->halted) return;

  uint64_t pc = cpu->pc;
  if (pc & 1) {
    RiscvTrap(cpu, s, kExFetchMisaligned, pc);
    return;
  }
  Bus *bus = cpu->bus;
  uint16_t half = (uint16_t)BusRead(bus, pc, 2);
  uint32_t inst;
  int ilen;
  int c_enabled = (int)((s->misa >> 2) & 1);
  if (c_enabled && (half & 3) != 3) {
    inst = half;
    ilen = 2;
  } else {
    // With misa.C clear IALIGN is 32: a 4-byte instruction at a
    // 2-aligned address is an instruction-address-misaligned exception.
    if (!c_enabled && pc % 4) {
      RiscvTrap(cpu, s, kExFetchMisaligned, pc);
      return;
    }
    inst = (uint32_t)BusRead(bus, pc, 4);
    ilen = 4;
  }
  RiscvTracePush(s, pc, inst, ilen);
  if (StepTrace()) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "pc=%016llx inst=%08x\n",
                     (unsigned long long)pc, inst);
    HostWriteErr(buf, (size_t)n);
  }
  uint64_t next_pc = pc + ilen;
  int trapped;
  if (ilen == 2)
    trapped = ExecCompressed(cpu, s, pc, (uint16_t)inst, &next_pc);
  else
    trapped = Exec32(cpu, s, pc, inst, &next_pc);
  if (!trapped) {
    cpu->pc = next_pc;
  }
  // Counter increments for this instruction, gated by mcountinhibit. A write
  // to a counter suppresses that counter's own increment for the writing
  // instruction (see RiscvState.counter_written).
  if (!(s->mcountinhibit & kMcountinhibitIr) &&
      s->counter_written != kCounterMinstret)
    s->minstret++;
  if (!(s->mcountinhibit & kMcountinhibitCy) &&
      s->counter_written != kCounterMcycle)
    s->mcycle++;
  s->counter_written = kCounterNone;
}
