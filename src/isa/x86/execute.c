#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x86.h"
#include "isa/isa.h"
#include "host/host.h"
#include "util/log.h"

// Multiboot boot loader magic the guest kernel expects in EAX on entry
// (Multiboot 0.6.96 spec, section 3.2); EBX carries a pointer to the boot
// information structure, which cemu does not build (guests with mb_flags=0,
// like kvm-unit-tests realmode, never read it).
enum {
  kMultibootHeaderMagic = 0x1BADB002,
  kMultibootLoaderMagic = 0x2BADB002,
  kMultibootInfoScratch = 0x7000,
};

// Flow of an executed instruction: either eip advances by the instruction
// length, or the instruction redirected it itself (control flow, trap).
enum { kFlowNext = 0, kFlowRedirect = 1 };

enum { kVecDe = 0, kVecUd = 6 };

// ---- register access ----
// size is the operand width in bytes: 1, 2 or 4. Byte registers 4-7 are the
// high halves of EAX..EBX.

static uint32_t RegRead(X86State *s, int reg, int size) {
  if (size == 4) return s->gpr[reg];
  if (size == 2) return s->gpr[reg] & 0xffff;
  return reg < 4 ? s->gpr[reg] & 0xff : (s->gpr[reg - 4] >> 8) & 0xff;
}

static void RegWrite(X86State *s, int reg, int size, uint32_t v) {
  if (size == 4) {
    s->gpr[reg] = v;
  } else if (size == 2) {
    s->gpr[reg] = (s->gpr[reg] & 0xffff0000) | (v & 0xffff);
  } else if (reg < 4) {
    s->gpr[reg] = (s->gpr[reg] & ~0xffu) | (v & 0xff);
  } else {
    s->gpr[reg - 4] = (s->gpr[reg - 4] & ~0xff00u) | ((v & 0xff) << 8);
  }
}

// AH/CH/DH/BH live at byte-register indices 4..7.
enum { kRegAh = 4 };

// ---- operand masks and flags ----

static uint32_t ValueMask(int size) {
  return size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1;
}

static uint32_t SignMask(int size) {
  return size == 4 ? 0x80000000u : 1u << (size * 8 - 1);
}

static int64_t Sext64(uint64_t v, int bits) {
  if (bits >= 64) return (int64_t)v;
  v &= (1ULL << bits) - 1;
  uint64_t sign = 1ULL << (bits - 1);
  return (int64_t)((v ^ sign) - sign);
}

static void SetPzsFlags(X86State *s, uint32_t res, int size) {
  int ones = 0;
  for (int i = 0; i < 8; i++) ones += (res >> i) & 1;
  s->eflags &= ~(kFlagPf | kFlagZf | kFlagSf);
  if (!(ones & 1)) s->eflags |= kFlagPf;
  if ((res & ValueMask(size)) == 0) s->eflags |= kFlagZf;
  if (res & SignMask(size)) s->eflags |= kFlagSf;
}

static void SetAddFlags(X86State *s, uint32_t a, uint32_t b, uint32_t res,
                        int size, int carry_in) {
  s->eflags &= ~(kFlagCf | kFlagOf | kFlagAf);
  if ((uint64_t)(a & ValueMask(size)) + (b & ValueMask(size)) + carry_in >
      ValueMask(size))
    s->eflags |= kFlagCf;
  if (((a ^ b ^ res) >> 4) & 1) s->eflags |= kFlagAf;
  if ((~(a ^ b) & (a ^ res)) & SignMask(size)) s->eflags |= kFlagOf;
  SetPzsFlags(s, res, size);
}

static void SetSubFlags(X86State *s, uint32_t a, uint32_t b, uint32_t res,
                        int size, int borrow_in) {
  s->eflags &= ~(kFlagCf | kFlagOf | kFlagAf);
  if ((uint64_t)(a & ValueMask(size)) <
      (uint64_t)(b & ValueMask(size)) + borrow_in)
    s->eflags |= kFlagCf;
  if (((a ^ b ^ res) >> 4) & 1) s->eflags |= kFlagAf;
  if ((a ^ b) & (a ^ res) & SignMask(size)) s->eflags |= kFlagOf;
  SetPzsFlags(s, res, size);
}

// AND/OR/XOR/TEST: CF = OF = AF = 0.
static void SetLogicFlags(X86State *s, uint32_t res, int size) {
  s->eflags &= ~(kFlagCf | kFlagOf | kFlagAf);
  SetPzsFlags(s, res, size);
}

// INC/DEC keep CF; overflow is a wraparound across the sign edge.
static void SetIncDecFlags(X86State *s, uint32_t res, int size, int is_inc) {
  s->eflags &= ~(kFlagOf | kFlagAf);
  if (res == (is_inc ? SignMask(size) : SignMask(size) - 1))
    s->eflags |= kFlagOf;
  if (is_inc ? (res & 0xf) == 0 : (res & 0xf) == 0xf) s->eflags |= kFlagAf;
  SetPzsFlags(s, res, size);
}

// ---- segments ----

// Real mode computes the segment base from the visible selector on every
// access; protected mode uses the descriptor cache (base/dbit). A descriptor
// cache loaded in protected mode therefore survives CR0.PE=0 (big real mode).
static uint64_t SegLinear(X86State *s, int seg) {
  if (s->cr0 & kCr0Pe) return s->base[seg];
  return (uint64_t)s->sreg[seg] << 4;
}

static void LoadSegment(CpuState *cpu, X86State *s, int seg, uint16_t sel) {
  s->sreg[seg] = sel;
  if (!(s->cr0 & kCr0Pe)) {
    s->base[seg] = (uint64_t)sel << 4;
    s->dbit[seg] = 0;
    return;
  }
  if (sel & 4) Fatal("x86: LDT segment selectors not implemented");
  // Descriptor walk without limit or privilege checks (registered scope).
  uint64_t desc = BusRead(cpu->bus, s->gdtr + ((sel >> 3) & 0x1fff) * 8, 8);
  // base = bytes[2..4] | bytes[7]; D/B = bit 6 of byte 6 (bit 54 overall).
  s->base[seg] = ((desc >> 16) & 0xffffff) | (((desc >> 56) & 0xff) << 24);
  s->dbit[seg] = (uint8_t)((desc >> 54) & 1);
}

// ---- instruction fetch ----

typedef struct Fetch {
  CpuState *cpu;
  uint64_t linear;  // CS base
  uint32_t ip;      // bytes consumed since the instruction start
} Fetch;

static uint8_t Fetch8(Fetch *f) {
  uint8_t v = (uint8_t)BusRead(f->cpu->bus, f->linear + f->ip, 1);
  f->ip++;
  return v;
}

static uint16_t Fetch16(Fetch *f) {
  uint16_t v = Fetch8(f);
  return v | (uint16_t)Fetch8(f) << 8;
}

static uint32_t Fetch32(Fetch *f) {
  uint32_t v = Fetch16(f);
  return v | (uint32_t)Fetch16(f) << 16;
}

static int8_t FetchS8(Fetch *f) { return (int8_t)Fetch8(f); }

static int16_t FetchS16(Fetch *f) { return (int16_t)Fetch16(f); }

static int32_t FetchS32(Fetch *f) { return (int32_t)Fetch32(f); }

static uint32_t FetchImm(Fetch *f, int size) {
  return size == 1 ? Fetch8(f) : size == 2 ? Fetch16(f) : Fetch32(f);
}

// Instruction prefixes gathered before the opcode.
typedef struct Prefixes {
  int opsz_toggle;  // 66: flips the default operand size
  int addr_toggle;  // 67: flips the default address size
  int seg;          // segment override index, -1 = none
  int rep;          // 0 = none, 1 = F3, 2 = F2 (string ops only)
} Prefixes;

// ---- modrm decoding ----
// Segment defaults: any BP/EBP-based addressing uses SS, everything else DS.
// A segment override prefix replaces the default for every memory reference
// of the instruction.

typedef struct Modrm {
  int mod, reg, rm;
  int is_mem;
  uint32_t off;  // offset within the segment
  int seg;       // segment index after applying the override
} Modrm;

static uint32_t Disp(Fetch *f, int addrsz, int mod) {
  if (mod == 1) return (uint32_t)(int8_t)Fetch8(f);
  return addrsz == 2 ? Fetch16(f) : Fetch32(f);
}

static void DecodeModrm(Fetch *f, X86State *s, const Prefixes *p, int addrsz,
                        Modrm *m) {
  uint8_t enc = Fetch8(f);
  m->mod = enc >> 6;
  m->reg = (enc >> 3) & 7;
  m->rm = enc & 7;
  m->seg = p->seg;
  if (m->mod == 3) {
    m->is_mem = 0;
    return;
  }
  m->is_mem = 1;
  uint32_t off = 0;
  if (addrsz == 2) {
    static const int kBase[8] = {kEbx, kEbx, kEbp, kEbp,
                                 kEsi, kEdi, kEbp, kEbx};
    static const int kIndex[8] = {kEsi, kEdi, -1, -1, -1, -1, -1, -1};
    off = s->gpr[kBase[m->rm]];
    if (kIndex[m->rm] >= 0) off += s->gpr[kIndex[m->rm]];
    if (m->mod != 0) {
      off += Disp(f, addrsz, m->mod);
    } else if (m->rm == 6) {
      off = Disp(f, addrsz, 0);  // [disp16], no base
    }
    if (m->seg < 0 &&
        (m->rm == 2 || m->rm == 3 || (m->rm == 6 && m->mod != 0)))
      m->seg = kSegSs;
  } else if (m->rm == 4) {
    uint8_t sib = Fetch8(f);
    int scale = 1 << (sib >> 6);
    int index = (sib >> 3) & 7;
    int base = sib & 7;
    if (base == 5 && m->mod == 0) {
      off = Fetch32(f);  // no base register, disp32
    } else {
      off = s->gpr[base];
      if (index != 4) off += (uint32_t)scale * s->gpr[index];  // 4 = none
      if (m->seg < 0 && base == 5) m->seg = kSegSs;  // EBP base
    }
    if (m->mod != 0) off += Disp(f, addrsz, m->mod);
  } else {
    if (m->rm == 5 && m->mod == 0) {
      off = Fetch32(f);  // disp32, no base
    } else {
      off = s->gpr[m->rm];
      if (m->mod != 0) off += Disp(f, addrsz, m->mod);
    }
    if (m->seg < 0 && m->rm == 5 && m->mod != 0) m->seg = kSegSs;
  }
  if (m->seg < 0) m->seg = kSegDs;
  m->off = off;
}

static uint64_t MemLinear(X86State *s, const Modrm *m) {
  return SegLinear(s, m->seg) + m->off;
}

static uint32_t RmRead(CpuState *cpu, X86State *s, const Modrm *m, int size) {
  if (!m->is_mem) return RegRead(s, m->rm, size);
  return (uint32_t)(BusRead(cpu->bus, MemLinear(s, m), size) & ValueMask(size));
}

static void RmWrite(CpuState *cpu, X86State *s, const Modrm *m, int size,
                    uint32_t v) {
  if (!m->is_mem) {
    RegWrite(s, m->rm, size, v);
    return;
  }
  BusWrite(cpu->bus, MemLinear(s, m), size, v & ValueMask(size));
}

// ---- stack ----

static void StackAdjust(X86State *s, int size, int delta) {
  if (size == 2)  // SP wraps at 64K, ESP[31:16] untouched
    s->gpr[kEsp] =
        (s->gpr[kEsp] & 0xffff0000) | ((s->gpr[kEsp] + delta) & 0xffff);
  else
    s->gpr[kEsp] += delta;
}

static uint32_t StackOffset(X86State *s, int size) {
  return size == 2 ? s->gpr[kEsp] & 0xffff : s->gpr[kEsp];
}

static void StackSetSp(X86State *s, int size, uint32_t sp) {
  if (size == 2)
    s->gpr[kEsp] = (s->gpr[kEsp] & 0xffff0000) | (sp & 0xffff);
  else
    s->gpr[kEsp] = sp;
}

static uint64_t StackLinear(X86State *s, int size) {
  return SegLinear(s, kSegSs) + StackOffset(s, size);
}

static void Push(CpuState *cpu, X86State *s, int size, uint32_t val) {
  StackAdjust(s, size, -size);
  BusWrite(cpu->bus, StackLinear(s, size), size, val & ValueMask(size));
}

static uint32_t Pop(CpuState *cpu, X86State *s, int size) {
  uint32_t v =
      (uint32_t)(BusRead(cpu->bus, StackLinear(s, size), size) & ValueMask(size));
  StackAdjust(s, size, size);
  return v;
}

// ---- traps and real-mode interrupt dispatch ----

// Delivers a vector through the real-mode IVT (IDTR base). Faults push the
// address of the faulting instruction, which is still in s->eip here.
static void DoInt(CpuState *cpu, X86State *s, int vec) {
  if (s->cr0 & kCr0Pe)
    Fatal("x86: exception in protected mode (outside stage-1.5 scope)");
  uint64_t tbl = s->idtr + (uint64_t)vec * 4;
  uint32_t off = BusRead(cpu->bus, tbl, 2);
  uint32_t seg = BusRead(cpu->bus, tbl + 2, 2);
  Push(cpu, s, 2, s->eflags | 2);
  Push(cpu, s, 2, s->sreg[kSegCs]);
  Push(cpu, s, 2, s->eip & 0xffff);
  s->eflags &= ~(kFlagIf | kFlagTf);
  LoadSegment(cpu, s, kSegCs, (uint16_t)seg);
  s->eip = off;
}

// ---- ALU ----

enum {
  kAluAdd = 0,
  kAluOr = 1,
  kAluAdc = 2,
  kAluSbb = 3,
  kAluAnd = 4,
  kAluSub = 5,
  kAluXor = 6,
  kAluCmp = 7,
};

// Executes an ALU operation and sets flags; the caller stores the result
// unless the operation is CMP (kAluCmp).
static uint32_t Alu(X86State *s, int op, uint32_t a, uint32_t b, int size) {
  int carry = (s->eflags & kFlagCf) != 0;
  uint32_t res = 0;
  switch (op) {
    case kAluAdd:
      res = (a + b) & ValueMask(size);
      SetAddFlags(s, a, b, res, size, 0);
      break;
    case kAluAdc:
      res = (a + b + carry) & ValueMask(size);
      SetAddFlags(s, a, b, res, size, carry);
      break;
    case kAluSub:
    case kAluCmp:
      res = (a - b) & ValueMask(size);
      SetSubFlags(s, a, b, res, size, 0);
      break;
    case kAluSbb:
      res = (a - b - carry) & ValueMask(size);
      SetSubFlags(s, a, b, res, size, carry);
      break;
    case kAluAnd:
      res = a & b;
      SetLogicFlags(s, res, size);
      break;
    case kAluOr:
      res = a | b;
      SetLogicFlags(s, res, size);
      break;
    default:  // kAluXor
      res = a ^ b;
      SetLogicFlags(s, res, size);
      break;
  }
  return res;
}

// ---- shifts and rotates (GRP2: reg field selects the operation) ----

enum {
  kShiftRol = 0,
  kShiftRor = 1,
  kShiftRcl = 2,
  kShiftRcr = 3,
  kShiftShl = 4,
  kShiftShr = 5,
  kShiftSar = 7,
};

static uint32_t Shift(X86State *s, int op, uint32_t v, uint32_t count,
                      int size) {
  uint32_t mask = ValueMask(size);
  int width = size * 8;
  v &= mask;
  count &= 31;
  if (count == 0) return v;  // no flags change on count 0
  uint32_t res = v;
  switch (op) {
    case kShiftRol:
      count %= width;
      res = ((v << count) | (v >> (width - count))) & mask;
      s->eflags &= ~(kFlagCf | kFlagOf);
      if (res & 1) s->eflags |= kFlagCf;
      if (!!(res & SignMask(size)) ^ !!(res & (SignMask(size) >> 1)))
        s->eflags |= kFlagOf;
      return res;
    case kShiftRor:
      count %= width;
      res = ((v >> count) | (v << (width - count))) & mask;
      s->eflags &= ~(kFlagCf | kFlagOf);
      if (res & SignMask(size)) s->eflags |= kFlagCf;
      if (!!(res & SignMask(size)) ^ !!(res & (SignMask(size) >> 1)))
        s->eflags |= kFlagOf;
      return res;
    case kShiftRcl: {  // rotate through carry on a width+1 bit ring
      uint64_t ring = ((uint64_t)!!(s->eflags & kFlagCf) << width) | v;
      count %= width + 1;
      ring = (ring << count) | (ring >> (width + 1 - count));
      s->eflags &= ~(kFlagCf | kFlagOf);
      if ((ring >> width) & 1) s->eflags |= kFlagCf;
      if (!!(ring & SignMask(size)) ^ !!((ring >> width) & 1))
        s->eflags |= kFlagOf;
      return (uint32_t)ring & mask;
    }
    case kShiftRcr: {
      uint64_t ring = ((uint64_t)!!(s->eflags & kFlagCf) << width) | v;
      count %= width + 1;
      ring = (ring >> count) | (ring << (width + 1 - count));
      s->eflags &= ~(kFlagCf | kFlagOf);
      if ((ring >> width) & 1) s->eflags |= kFlagCf;
      if (!!(ring & SignMask(size)) ^ !!((ring >> width) & 1))
        s->eflags |= kFlagOf;
      return (uint32_t)ring & mask;
    }
    case kShiftShl:
      res = count >= (uint32_t)width ? 0 : (v << count) & mask;
      s->eflags &= ~(kFlagCf | kFlagOf | kFlagAf);
      if (count < (uint32_t)width && (v >> (width - count)) & 1)
        s->eflags |= kFlagCf;
      if (!!(res & SignMask(size)) ^ !!(v & (SignMask(size) >> 1)))
        s->eflags |= kFlagOf;
      SetPzsFlags(s, res, size);
      return res;
    case kShiftShr:
      res = v >> count;
      s->eflags &= ~(kFlagCf | kFlagOf | kFlagAf);
      if ((v >> (count - 1)) & 1) s->eflags |= kFlagCf;
      if (count == 1 && (v & SignMask(size))) s->eflags |= kFlagOf;
      SetPzsFlags(s, res, size);
      return res;
    default: {  // kShiftSar
      uint32_t fill = (v & SignMask(size)) ? mask : 0;
      res = count >= (uint32_t)width
                ? fill
                : ((v >> count) | (fill << (width - count))) & mask;
      s->eflags &= ~(kFlagCf | kFlagOf | kFlagAf);
      if ((count >= (uint32_t)width ? v : v >> (count - 1)) & SignMask(size))
        s->eflags |= kFlagCf;
      SetPzsFlags(s, res, size);
      return res;
    }
  }
}

// ---- conditions (shared by Jcc short/near and LOOPcc) ----

static int Cond(X86State *s, int c) {
  uint32_t f = s->eflags;
  int res;
  switch (c & ~1) {
    case 0x0: res = !!(f & kFlagOf); break;
    case 0x2: res = !!(f & kFlagCf); break;
    case 0x4: res = !!(f & kFlagZf); break;
    case 0x6: res = !!(f & (kFlagCf | kFlagZf)); break;
    case 0x8: res = !!(f & kFlagSf); break;
    case 0xa: res = !!(f & kFlagPf); break;
    case 0xc: res = !!(f & kFlagSf) != !!(f & kFlagOf); break;
    default:  // 0xe
      res = !!(f & kFlagZf) || !!(f & kFlagSf) != !!(f & kFlagOf);
      break;
  }
  return res ^ (c & 1);
}

// ---- BCD adjust (DAA/DAS/AAA/AAS), per the SDM algorithms ----

static void ExecDaa(X86State *s) {
  uint32_t al = RegRead(s, kEax, 1);
  int old_cf = !!(s->eflags & kFlagCf);
  int cf = 0;
  if ((al & 0xf) > 9 || (s->eflags & kFlagAf)) {
    cf = old_cf || al > 0xf9;  // AL + 6 carries out of the low byte
    al += 6;
    s->eflags |= kFlagAf;
  } else {
    s->eflags &= ~kFlagAf;
  }
  if (al > 0x99 || old_cf) {
    al += 0x60;
    cf = 1;
  }
  RegWrite(s, kEax, 1, al);
  SetLogicFlags(s, al, 1);
  if (cf) s->eflags |= kFlagCf;
}

static void ExecDas(X86State *s) {
  uint32_t al = RegRead(s, kEax, 1);
  int old_cf = !!(s->eflags & kFlagCf);
  int cf = 0;
  if ((al & 0xf) > 9 || (s->eflags & kFlagAf)) {
    cf = old_cf || al < 6;  // AL - 6 borrows
    al -= 6;
    s->eflags |= kFlagAf;
  } else {
    s->eflags &= ~kFlagAf;
  }
  if (al > 0x99 || old_cf) {
    al -= 0x60;
    cf = 1;
  }
  RegWrite(s, kEax, 1, al);
  SetLogicFlags(s, al, 1);
  if (cf) s->eflags |= kFlagCf;
}

static void ExecAaa(X86State *s) {
  if ((RegRead(s, kEax, 1) & 0xf) > 9 || (s->eflags & kFlagAf)) {
    uint32_t ax = (RegRead(s, kEax, 1) & 0xffff) + 0x106;
    RegWrite(s, kEax, 2, ax);
    s->eflags |= kFlagAf | kFlagCf;
  } else {
    s->eflags &= ~(kFlagAf | kFlagCf);
  }
  RegWrite(s, kEax, 1, RegRead(s, kEax, 1) & 0xf);
}

static void ExecAas(X86State *s) {
  if ((RegRead(s, kEax, 1) & 0xf) > 9 || (s->eflags & kFlagAf)) {
    RegWrite(s, kEax, 2, RegRead(s, kEax, 2) - 6);
    RegWrite(s, kEax + 4, 1, RegRead(s, kEax + 4, 1) - 1);  // AH -= 1
    s->eflags |= kFlagAf | kFlagCf;
  } else {
    s->eflags &= ~(kFlagAf | kFlagCf);
  }
  RegWrite(s, kEax, 1, RegRead(s, kEax, 1) & 0xf);
}

// ---- multiply and divide (GRP3 /4-/7) ----

// Returns 0 normally, -1 when a #DE (divide error) was raised.
static int ExecMulDiv(CpuState *cpu, X86State *s, int op, int size,
                      uint32_t src) {
  enum { kMul = 4, kImul = 5, kDiv = 6, kIdiv = 7 };
  int bits = size * 8;
  if (size == 1) {
    uint32_t ax = s->gpr[kEax] & 0xffff;
    switch (op) {
      case kMul: {  // AX = AL * src8
        uint32_t res = ax * src;
        s->gpr[kEax] = (s->gpr[kEax] & 0xffff0000) | (res & 0xffff);
        s->eflags &= ~(kFlagCf | kFlagOf);
        if (res >> 8) s->eflags |= kFlagCf | kFlagOf;
        return 0;
      }
      case kImul: {  // AX = sext8(AL) * sext8(src)
        int16_t prod = (int16_t)(int8_t)(ax & 0xff) * (int8_t)src;
        s->gpr[kEax] = (s->gpr[kEax] & 0xffff0000) | (uint16_t)prod;
        s->eflags &= ~(kFlagCf | kFlagOf);
        if (prod != (int8_t)prod) s->eflags |= kFlagCf | kFlagOf;
        return 0;
      }
      case kDiv: {  // AL = AX / src8, AH = remainder
        if (src == 0 || ax / src > 0xff) break;
        uint32_t quot = ax / src, rem = ax % src;
        s->gpr[kEax] = (s->gpr[kEax] & 0xffff0000) | (quot & 0xff) |
                       ((rem & 0xff) << 8);
        return 0;
      }
      default: {  // kIdiv: dividend is the full AX
        int16_t dividend = (int16_t)ax;
        if (src == 0) break;
        int16_t quot = dividend / (int8_t)src;
        if (quot != (int8_t)quot) break;
        uint32_t rem = (uint32_t)(dividend % (int8_t)src) & 0xff;
        s->gpr[kEax] = (s->gpr[kEax] & 0xffff0000) | ((uint32_t)quot & 0xff) |
                       (rem << 8);
        return 0;
      }
    }
    DoInt(cpu, s, kVecDe);  // #DE: quotient out of range or division by zero
    return -1;
  }
  // 16/32-bit: dividend is the register pair (DX:AX or EDX:EAX).
  uint64_t upair = ((uint64_t)(s->gpr[kEdx] & ValueMask(size)) << bits) |
                   (s->gpr[kEax] & ValueMask(size));
  uint64_t usrc = src & ValueMask(size);
  switch (op) {
    case kMul: {
      uint64_t prod = (uint64_t)(s->gpr[kEax] & ValueMask(size)) * usrc;
      s->gpr[kEax] = (uint32_t)prod;
      s->gpr[kEdx] = (uint32_t)(prod >> bits);
      s->eflags &= ~(kFlagCf | kFlagOf);
      if (prod >> bits) s->eflags |= kFlagCf | kFlagOf;
      return 0;
    }
    case kImul: {
      int64_t prod = Sext64(s->gpr[kEax], bits) * Sext64(src, bits);
      s->gpr[kEax] = (uint32_t)prod;
      s->gpr[kEdx] = (uint32_t)((uint64_t)prod >> bits);
      s->eflags &= ~(kFlagCf | kFlagOf);
      if (prod != Sext64((uint64_t)prod, bits)) s->eflags |= kFlagCf | kFlagOf;
      return 0;
    }
    case kDiv: {
      if (usrc == 0 || upair / usrc > ValueMask(size)) break;
      s->gpr[kEax] = (uint32_t)(upair / usrc);
      s->gpr[kEdx] = (uint32_t)(upair % usrc);
      return 0;
    }
    default: {  // kIdiv
      if (usrc == 0) break;
      int64_t dividend =
          size == 2 ? Sext64(upair, 32) : (int64_t)upair;
      int64_t divisor = Sext64(src, bits);
      int64_t quot = dividend / divisor;
      if (quot < -(int64_t)(1LL << (bits - 1)) ||
          quot >= (int64_t)(1LL << (bits - 1)))
        break;
      s->gpr[kEax] = (uint32_t)quot;
      s->gpr[kEdx] = (uint32_t)(dividend % divisor);
      return 0;
    }
  }
  DoInt(cpu, s, kVecDe);
  return -1;
}

// ---- debug output ----

void X86DumpRegs(const CpuState *cpu) {
  const X86State *s = (const X86State *)cpu->priv;
  char buf[160];
  static const char names[8][4] = {"EAX", "ECX", "EDX", "EBX",
                                   "ESP", "EBP", "ESI", "EDI"};
  for (int i = 0; i < 8; i += 4) {
    int n = snprintf(buf, sizeof(buf), "%s=%08x %s=%08x %s=%08x %s=%08x\n",
                     names[i], s->gpr[i], names[i + 1], s->gpr[i + 1],
                     names[i + 2], s->gpr[i + 2], names[i + 3], s->gpr[i + 3]);
    HostWriteErr(buf, (size_t)n);
  }
  static const char snames[6][4] = {"ES", "CS", "SS", "DS", "FS", "GS"};
  int n = snprintf(
      buf, sizeof(buf),
      "EIP=%08x EFLAGS=%08x CR0=%08x %s=%04x %s=%04x %s=%04x %s=%04x %s=%04x "
      "%s=%04x\n",
      s->eip, s->eflags, s->cr0, snames[0], s->sreg[0], snames[1], s->sreg[1],
      snames[2], s->sreg[2], snames[3], s->sreg[3], snames[4], s->sreg[4],
      snames[5], s->sreg[5]);
  HostWriteErr(buf, (size_t)n);
}

// ---- reset / boot ----

void X86Init(CpuState *cpu) {
  X86State *s = (X86State *)cpu->priv;
  if (!s) {
    s = (X86State *)calloc(1, sizeof(X86State));
    cpu->priv = s;
  } else {
    memset(s, 0, sizeof(*s));
  }
  s->eip = (uint32_t)cpu->pc;
  s->eflags = 0x202;  // IF set, bit 1 forced on
  // Detect a multiboot image (header within the first 8 KiB of the load
  // region, 4-byte aligned). QEMU enters such kernels in flat 32-bit
  // protected mode; everything else is a BIOS boot sector.
  uint64_t scan = cpu->pc & ~0xfffull;
  int multiboot = 0;
  for (uint64_t a = scan; a < scan + 0x2000; a += 4) {
    if (BusRead(cpu->bus, a, 4) == kMultibootHeaderMagic) {
      multiboot = 1;
      break;
    }
  }
  if (multiboot) {
    // QEMU multiboot entry state: flat 32-bit segments (CS=0x08, data
    // selectors 0x10), PE on. The initial descriptors are built in, so no
    // GDT needs to exist in RAM before the guest's own lgdt.
    s->cr0 = kCr0Pe;
    s->sreg[kSegCs] = 0x08;
    s->base[kSegCs] = 0;
    s->dbit[kSegCs] = 1;
    for (int i = 0; i < kSegCount; i++) {
      if (i == kSegCs) continue;
      s->sreg[i] = 0x10;
      s->base[i] = 0;
      s->dbit[i] = 1;
    }
    s->gpr[kEax] = kMultibootLoaderMagic;
    s->gpr[kEbx] = kMultibootInfoScratch;
  } else {
    // BIOS boot-sector handoff: CS:IP = 0000:7C00, DL = 0x80 (boot drive).
    s->gpr[kEdx] = 0x80;
    s->idtr_limit = 0x3ff;  // real-mode IVT at linear 0
  }
}

// ---- instruction execution ----

static uint32_t RelTarget(uint32_t ip_after, int64_t rel, int size) {
  if (size == 2)
    return (uint32_t)(((ip_after & 0xffff) + (int16_t)rel) & 0xffff);
  return ip_after + (uint32_t)(int32_t)rel;
}

// Far-pointer loads (LDS/LSS/LES/LFS/LGS): offset then selector in memory.
static void LoadFar(CpuState *cpu, X86State *s, const Modrm *m, int size,
                    int seg) {
  uint32_t off = RmRead(cpu, s, m, size);
  uint16_t sel = (uint16_t)BusRead(cpu->bus, MemLinear(s, m) + size, 2);
  RegWrite(s, m->reg, size, off);
  LoadSegment(cpu, s, seg, sel);
}

void X86Step(CpuState *cpu) {
  X86State *s = (X86State *)cpu->priv;
  if (!cpu->io) Fatal("x86 requires a machine with port I/O");

  int flow = kFlowNext;
  Fetch f = {cpu, SegLinear(s, kSegCs), s->eip};
  Prefixes p = {0, 0, -1, 0};
  uint8_t opcode;
  for (;;) {
    uint8_t b = Fetch8(&f);
    if (b == 0x66) p.opsz_toggle = 1;
    else if (b == 0x67) p.addr_toggle = 1;
    else if (b == 0x26) p.seg = kSegEs;
    else if (b == 0x2e) p.seg = kSegCs;
    else if (b == 0x36) p.seg = kSegSs;
    else if (b == 0x3e) p.seg = kSegDs;
    else if (b == 0x64) p.seg = kSegFs;
    else if (b == 0x65) p.seg = kSegGs;
    else if (b == 0xf0) {}  // LOCK: single hart, nothing to lock
    else if (b == 0xf2) p.rep = 2;
    else if (b == 0xf3) p.rep = 1;
    else { opcode = b; break; }
  }
  int two_byte = opcode == 0x0f;
  uint8_t op2 = two_byte ? Fetch8(&f) : 0;
  int opsz = ((s->cr0 & kCr0Pe) && s->dbit[kSegCs]) ? 4 : 2;
  if (p.opsz_toggle) opsz = 6 - opsz;
  int addrsz = ((s->cr0 & kCr0Pe) && s->dbit[kSegCs]) ? 4 : 2;
  if (p.addr_toggle) addrsz = 6 - addrsz;

  Modrm m;
  if (two_byte) {
    switch (op2) {
      case 0x01: {  // GRP6: descriptor table registers
        DecodeModrm(&f, s, &p, addrsz, &m);
        switch (m.reg) {
          case 0: case 1: {  // SGDT/SIDT: store limit(2) + base(4)
            uint64_t lin = MemLinear(s, &m);
            uint64_t base = m.reg == 0 ? s->gdtr : s->idtr;
            uint32_t limit = m.reg == 0 ? s->gdtr_limit : s->idtr_limit;
            BusWrite(cpu->bus, lin, 2, limit);
            BusWrite(cpu->bus, lin + 2, 4, base);
            break;
          }
          case 2: case 3: {  // LGDT/LIDT
            uint64_t lin = MemLinear(s, &m);
            uint16_t limit = (uint16_t)BusRead(cpu->bus, lin, 2);
            uint64_t base = BusRead(cpu->bus, lin + 2, 4);
            if (m.reg == 2) {
              s->gdtr = base;
              s->gdtr_limit = limit;
            } else {
              s->idtr = base;
              s->idtr_limit = limit;
            }
            break;
          }
          case 4:  // SMSW: store CR0 (low 16 bits with a 16-bit operand)
            RmWrite(cpu, s, &m, opsz, s->cr0);
            break;
          default:
            goto illegal;  // LMSW/INVLPG: stage-4 scope
        }
        break;
      }
      case 0x02:  // LAR/LSL: protection checks, not in scope
      case 0x03:
        goto illegal;
      case 0x0b:  // UD2
        goto illegal;
      case 0x1f:  // multi-byte NOP
        DecodeModrm(&f, s, &p, addrsz, &m);
        break;
      case 0x20: {  // MOV r, CR0
        DecodeModrm(&f, s, &p, addrsz, &m);
        if (m.reg != 0) goto illegal;
        RegWrite(s, m.rm, 4, s->cr0);
        break;
      }
      case 0x22: {  // MOV CR0, r (PE on/off drives the mode switches)
        DecodeModrm(&f, s, &p, addrsz, &m);
        if (m.reg != 0) goto illegal;
        s->cr0 = RegRead(s, m.rm, 4);
        break;
      }
      case 0x21: {  // MOV r, DRn
        DecodeModrm(&f, s, &p, addrsz, &m);
        RegWrite(s, m.rm, 4, s->dr[m.reg & 7]);
        break;
      }
      case 0x23: {  // MOV DRn, r
        DecodeModrm(&f, s, &p, addrsz, &m);
        s->dr[m.reg & 7] = RegRead(s, m.rm, 4);
        break;
      }
      case 0x31:  // RDTSC: no cycle counter in this machine
        goto illegal;
      case 0x80: case 0x81: case 0x82: case 0x83:
      case 0x84: case 0x85: case 0x86: case 0x87:
      case 0x88: case 0x89: case 0x8a: case 0x8b:
      case 0x8c: case 0x8d: case 0x8e: case 0x8f: {  // Jcc near
        int64_t rel = opsz == 2 ? FetchS16(&f) : FetchS32(&f);
        if (Cond(s, op2 & 0xf)) {
          f.ip = RelTarget(f.ip, rel, opsz);
          flow = kFlowRedirect;
        }
        break;
      }
      case 0x90: case 0x91: case 0x92: case 0x93:
      case 0x94: case 0x95: case 0x96: case 0x97:
      case 0x98: case 0x99: case 0x9a: case 0x9b:
      case 0x9c: case 0x9d: case 0x9e: case 0x9f: {  // SETcc rm8
        DecodeModrm(&f, s, &p, addrsz, &m);
        RmWrite(cpu, s, &m, 1, Cond(s, op2 & 0xf) ? 1 : 0);
        break;
      }
      case 0xa0: Push(cpu, s, opsz, s->sreg[kSegFs]); break;
      case 0xa1:
        LoadSegment(cpu, s, kSegFs, (uint16_t)Pop(cpu, s, opsz));
        break;
      case 0xa8: Push(cpu, s, opsz, s->sreg[kSegGs]); break;
      case 0xa9:
        LoadSegment(cpu, s, kSegGs, (uint16_t)Pop(cpu, s, opsz));
        break;
      case 0xa2: {  // CPUID
        uint32_t leaf = s->gpr[kEax];
        if (leaf == 0) {
          s->gpr[kEax] = 1;  // highest leaf
          s->gpr[kEbx] = 0x756e6547;  // "uneG"
          s->gpr[kEdx] = 0x49656e69;  // "Ieni"
          s->gpr[kEcx] = 0x6c65746e;  // "letn" (GenuineIntel)
        } else if (leaf == 1) {
          // Family 6, model 3, stepping 3. No feature bits: this machine has
          // neither an FPU nor a TSC.
          s->gpr[kEax] = 0x633;
          s->gpr[kEbx] = s->gpr[kEcx] = s->gpr[kEdx] = 0;
        } else {
          s->gpr[kEax] = s->gpr[kEbx] = s->gpr[kEcx] = s->gpr[kEdx] = 0;
        }
        break;
      }
      case 0xa3: case 0xab: case 0xb3: case 0xbb: {  // BT/BTS/BTR/BTC rm, r
        DecodeModrm(&f, s, &p, addrsz, &m);
        uint32_t bit = RegRead(s, m.reg, opsz);
        uint32_t v = RmRead(cpu, s, &m, opsz);
        uint32_t pos = bit & (opsz * 8 - 1);
        s->eflags &= ~kFlagCf;
        if ((v >> pos) & 1) s->eflags |= kFlagCf;
        if (op2 == 0xab) RmWrite(cpu, s, &m, opsz, v | (1u << pos));
        else if (op2 == 0xb3) RmWrite(cpu, s, &m, opsz, v & ~(1u << pos));
        else if (op2 == 0xbb) RmWrite(cpu, s, &m, opsz, v ^ (1u << pos));
        break;
      }
      case 0xa4: case 0xa5: {  // SHLD r/m, reg, imm8/CL
        DecodeModrm(&f, s, &p, addrsz, &m);
        uint32_t count = (op2 == 0xa4 ? Fetch8(&f) : RegRead(s, kEcx, 1)) & 31;
        if (count) {
          uint64_t comb = ((uint64_t)RmRead(cpu, s, &m, opsz) << (opsz * 8)) |
                          RegRead(s, m.reg, opsz);
          uint32_t res =
              (uint32_t)((comb << count) >> (opsz * 8)) & ValueMask(opsz);
          s->eflags &= ~(kFlagCf | kFlagOf | kFlagAf);
          if ((comb >> (2 * opsz * 8 - count)) & 1) s->eflags |= kFlagCf;
          SetPzsFlags(s, res, opsz);
          RmWrite(cpu, s, &m, opsz, res);
        }
        break;
      }
      case 0xac: case 0xad: {  // SHRD r/m, reg, imm8/CL
        DecodeModrm(&f, s, &p, addrsz, &m);
        uint32_t count = (op2 == 0xac ? Fetch8(&f) : RegRead(s, kEcx, 1)) & 31;
        if (count) {
          uint64_t comb = ((uint64_t)RmRead(cpu, s, &m, opsz) << (opsz * 8)) |
                          RegRead(s, m.reg, opsz);
          uint32_t res = (uint32_t)(comb >> count) & ValueMask(opsz);
          s->eflags &= ~(kFlagCf | kFlagOf | kFlagAf);
          if ((comb >> (count - 1)) & 1) s->eflags |= kFlagCf;
          SetPzsFlags(s, res, opsz);
          RmWrite(cpu, s, &m, opsz, res);
        }
        break;
      }
      case 0xaf: {  // IMUL r, r/m
        DecodeModrm(&f, s, &p, addrsz, &m);
        int64_t prod = Sext64(RegRead(s, m.reg, opsz), opsz * 8) *
                       Sext64(RmRead(cpu, s, &m, opsz), opsz * 8);
        RegWrite(s, m.reg, opsz, (uint32_t)prod);
        s->eflags &= ~(kFlagCf | kFlagOf);
        if (prod != Sext64((uint64_t)prod, opsz * 8))
          s->eflags |= kFlagCf | kFlagOf;
        break;
      }
      case 0xb0: case 0xb1:  // CMPXCHG: 486+, not in scope
        goto illegal;
      case 0xb2:  // LSS r, m16:32
        DecodeModrm(&f, s, &p, addrsz, &m);
        if (!m.is_mem) goto illegal;
        LoadFar(cpu, s, &m, opsz, kSegSs);
        break;
      case 0xb4:  // LFS r, m16:32
        DecodeModrm(&f, s, &p, addrsz, &m);
        if (!m.is_mem) goto illegal;
        LoadFar(cpu, s, &m, opsz, kSegFs);
        break;
      case 0xb5:  // LGS r, m16:32
        DecodeModrm(&f, s, &p, addrsz, &m);
        if (!m.is_mem) goto illegal;
        LoadFar(cpu, s, &m, opsz, kSegGs);
        break;
      case 0xb6: case 0xb7: {  // MOVZX r, rm8 / rm16
        DecodeModrm(&f, s, &p, addrsz, &m);
        RegWrite(s, m.reg, opsz, RmRead(cpu, s, &m, op2 == 0xb6 ? 1 : 2));
        break;
      }
      case 0xbe: case 0xbf: {  // MOVSX r, rm8 / rm16
        DecodeModrm(&f, s, &p, addrsz, &m);
        int srcsz = op2 == 0xbe ? 1 : 2;
        RegWrite(s, m.reg, opsz, (uint32_t)Sext64(RmRead(cpu, s, &m, srcsz),
                                                 srcsz * 8));
        break;
      }
      case 0xc0: case 0xc1: {  // XADD r/m, r (486+; exercised by realmode)
        DecodeModrm(&f, s, &p, addrsz, &m);
        uint32_t dst = RmRead(cpu, s, &m, opsz);
        uint32_t src = RegRead(s, m.reg, opsz);
        RmWrite(cpu, s, &m, opsz, Alu(s, kAluAdd, dst, src, opsz));
        RegWrite(s, m.reg, opsz, dst);
        break;
      }
      case 0xc8: case 0xc9: case 0xca: case 0xcb:
      case 0xcc: case 0xcd: case 0xce: case 0xcf: {  // BSWAP r32
        uint32_t v = RegRead(s, op2 & 7, 4);
        RegWrite(s, op2 & 7, 4, ((v & 0xff) << 24) | ((v & 0xff00) << 8) |
                                    ((v >> 8) & 0xff00) | (v >> 24));
        break;
      }
      default:
        goto illegal;
    }
  } else if (opcode < 0x40 && (opcode & 7) < 6) {
    // ALU family: opcode = op<<3 | form.
    int alu = opcode >> 3;
    int form = opcode & 7;
    uint32_t dst, src, res;
    int size;
    switch (form) {
      case 0:  // r/m8, r8
        DecodeModrm(&f, s, &p, addrsz, &m);
        size = 1;
        dst = RmRead(cpu, s, &m, 1);
        src = RegRead(s, m.reg, 1);
        break;
      case 1:  // r/m, r
        DecodeModrm(&f, s, &p, addrsz, &m);
        size = opsz;
        dst = RmRead(cpu, s, &m, opsz);
        src = RegRead(s, m.reg, opsz);
        break;
      case 2:  // r8, r/m8
        DecodeModrm(&f, s, &p, addrsz, &m);
        size = 1;
        dst = RegRead(s, m.reg, 1);
        src = RmRead(cpu, s, &m, 1);
        break;
      case 3:  // r, r/m
        DecodeModrm(&f, s, &p, addrsz, &m);
        size = opsz;
        dst = RegRead(s, m.reg, opsz);
        src = RmRead(cpu, s, &m, opsz);
        break;
      case 4:  // AL, imm8
        size = 1;
        dst = RegRead(s, kEax, 1);
        src = Fetch8(&f);
        break;
      default:  // 5: eAX, imm
        size = opsz;
        dst = RegRead(s, kEax, opsz);
        src = FetchImm(&f, opsz);
        break;
    }
    res = Alu(s, alu, dst, src, size);
    if (alu != kAluCmp) {
      if (form <= 1) RmWrite(cpu, s, &m, size, res);
      else if (form <= 3) RegWrite(s, m.reg, size, res);
      else RegWrite(s, kEax, size, res);
    }
  } else {
    switch (opcode) {
      case 0x06: case 0x0e: case 0x16: case 0x1e:  // PUSH sreg
        Push(cpu, s, opsz, s->sreg[opcode >> 3]);
        break;
      case 0x07: case 0x17: case 0x1f:  // POP sreg (POP CS is 8086 only)
        LoadSegment(cpu, s, opcode >> 3, (uint16_t)Pop(cpu, s, opsz));
        break;
      case 0x27:  // DAA
        ExecDaa(s);
        break;
      case 0x2f:  // DAS
        ExecDas(s);
        break;
      case 0x37:  // AAA
        ExecAaa(s);
        break;
      case 0x3f:  // AAS
        ExecAas(s);
        break;
      case 0x40: case 0x41: case 0x42: case 0x43:
      case 0x44: case 0x45: case 0x46: case 0x47: {  // INC r
        uint32_t res = (RegRead(s, opcode & 7, opsz) + 1) & ValueMask(opsz);
        SetIncDecFlags(s, res, opsz, 1);
        RegWrite(s, opcode & 7, opsz, res);
        break;
      }
      case 0x48: case 0x49: case 0x4a: case 0x4b:
      case 0x4c: case 0x4d: case 0x4e: case 0x4f: {  // DEC r
        uint32_t res = (RegRead(s, opcode & 7, opsz) - 1) & ValueMask(opsz);
        SetIncDecFlags(s, res, opsz, 0);
        RegWrite(s, opcode & 7, opsz, res);
        break;
      }
      case 0x50: case 0x51: case 0x52: case 0x53:
      case 0x54: case 0x55: case 0x56: case 0x57:  // PUSH r
        Push(cpu, s, opsz, RegRead(s, opcode & 7, opsz));
        break;
      case 0x58: case 0x59: case 0x5a: case 0x5b:
      case 0x5c: case 0x5d: case 0x5e: case 0x5f:  // POP r
        RegWrite(s, opcode & 7, opsz, Pop(cpu, s, opsz));
        break;
      case 0x60: {  // PUSHA / PUSHAD
        uint32_t sp = RegRead(s, kEsp, opsz);
        for (int i = 0; i < 8; i++)
          Push(cpu, s, opsz, i == 4 ? sp : RegRead(s, i, opsz));
        break;
      }
      case 0x61:  // POPA / POPAD: DI SI BP (skip SP) BX DX CX AX
        for (int i = 7; i >= 0; i--) {
          uint32_t v = Pop(cpu, s, opsz);
          if (i != 4) RegWrite(s, i, opsz, v);
        }
        break;
      case 0x68:  // PUSH imm
        Push(cpu, s, opsz, FetchImm(&f, opsz));
        break;
      case 0x69: {  // IMUL r, r/m, imm
        DecodeModrm(&f, s, &p, addrsz, &m);
        uint32_t imm = FetchImm(&f, opsz);
        int64_t prod = Sext64(RmRead(cpu, s, &m, opsz), opsz * 8) *
                       Sext64(imm, opsz * 8);
        RegWrite(s, m.reg, opsz, (uint32_t)prod);
        s->eflags &= ~(kFlagCf | kFlagOf);
        if (prod != Sext64((uint64_t)prod, opsz * 8))
          s->eflags |= kFlagCf | kFlagOf;
        break;
      }
      case 0x6a:  // PUSH imm8 (sign-extended)
        Push(cpu, s, opsz, (uint32_t)Sext64(Fetch8(&f), 8));
        break;
      case 0x6b: {  // IMUL r, r/m, imm8
        DecodeModrm(&f, s, &p, addrsz, &m);
        int64_t imm = (int8_t)Fetch8(&f);
        int64_t prod = Sext64(RmRead(cpu, s, &m, opsz), opsz * 8) * imm;
        RegWrite(s, m.reg, opsz, (uint32_t)prod);
        s->eflags &= ~(kFlagCf | kFlagOf);
        if (prod != Sext64((uint64_t)prod, opsz * 8))
          s->eflags |= kFlagCf | kFlagOf;
        break;
      }
      case 0x70: case 0x71: case 0x72: case 0x73:
      case 0x74: case 0x75: case 0x76: case 0x77:
      case 0x78: case 0x79: case 0x7a: case 0x7b:
      case 0x7c: case 0x7d: case 0x7e: case 0x7f: {  // Jcc rel8
        int8_t rel = FetchS8(&f);
        if (Cond(s, opcode & 0xf)) {
          f.ip = RelTarget(f.ip, rel, opsz);
          flow = kFlowRedirect;
        }
        break;
      }
      case 0x80: case 0x81: case 0x82: case 0x83: {  // GRP1
        DecodeModrm(&f, s, &p, addrsz, &m);
        int size = opcode == 0x81 || opcode == 0x83 ? opsz : 1;
        uint32_t src;
        if (opcode == 0x81) {
          src = FetchImm(&f, opsz);
        } else if (opcode == 0x83) {
          src = (uint32_t)Sext64(Fetch8(&f), 8);  // imm8 sign-extended
        } else {
          src = Fetch8(&f);
        }
        uint32_t res = Alu(s, m.reg, RmRead(cpu, s, &m, size), src, size);
        if (m.reg != kAluCmp) RmWrite(cpu, s, &m, size, res);
        break;
      }
      case 0x84: case 0x85: {  // TEST r/m, r
        DecodeModrm(&f, s, &p, addrsz, &m);
        int size = opcode == 0x85 ? opsz : 1;
        SetLogicFlags(s, RmRead(cpu, s, &m, size) & RegRead(s, m.reg, size),
                      size);
        break;
      }
      case 0x86: case 0x87: {  // XCHG r/m, r
        DecodeModrm(&f, s, &p, addrsz, &m);
        int size = opcode == 0x87 ? opsz : 1;
        uint32_t t = RmRead(cpu, s, &m, size);
        RmWrite(cpu, s, &m, size, RegRead(s, m.reg, size));
        RegWrite(s, m.reg, size, t);
        break;
      }
      case 0x88: case 0x89: case 0x8a: case 0x8b: {  // MOV r/m, r / r, r/m
        DecodeModrm(&f, s, &p, addrsz, &m);
        int size = (opcode & 1) ? opsz : 1;
        if (opcode & 2)
          RegWrite(s, m.reg, size, RmRead(cpu, s, &m, size));
        else
          RmWrite(cpu, s, &m, size, RegRead(s, m.reg, size));
        break;
      }
      case 0x8c: {  // MOV r/m16, sreg
        DecodeModrm(&f, s, &p, addrsz, &m);
        if (m.reg > kSegGs) goto illegal;
        RmWrite(cpu, s, &m, opsz, s->sreg[m.reg]);
        break;
      }
      case 0x8d: {  // LEA r, m
        DecodeModrm(&f, s, &p, addrsz, &m);
        if (!m.is_mem) goto illegal;
        RegWrite(s, m.reg, opsz, m.off);
        break;
      }
      case 0x8e: {  // MOV sreg, r/m16
        DecodeModrm(&f, s, &p, addrsz, &m);
        if (m.reg > kSegGs) goto illegal;
        LoadSegment(cpu, s, m.reg, (uint16_t)RmRead(cpu, s, &m, 2));
        break;
      }
      case 0x8f: {  // POP r/m16
        DecodeModrm(&f, s, &p, addrsz, &m);
        uint32_t v = Pop(cpu, s, opsz);
        RmWrite(cpu, s, &m, opsz, v);
        break;
      }
      case 0x90:  // NOP
        break;
      case 0x91: case 0x92: case 0x93: case 0x94:
      case 0x95: case 0x96: case 0x97: {  // XCHG AX, r
        uint32_t t = RegRead(s, kEax, opsz);
        RegWrite(s, kEax, opsz, RegRead(s, opcode & 7, opsz));
        RegWrite(s, opcode & 7, opsz, t);
        break;
      }
      case 0x98: {  // CBW / CWDE
        if (opsz == 2)
          RegWrite(s, kEax, 2, (uint32_t)Sext64(RegRead(s, kEax, 1), 8));
        else
          RegWrite(s, kEax, 4, (uint32_t)Sext64(RegRead(s, kEax, 2), 16));
        break;
      }
      case 0x99: {  // CWD / CDQ
        uint32_t sign = RegRead(s, kEax, opsz) & SignMask(opsz) ? 0xffffffffu
                                                                : 0;
        RegWrite(s, kEdx, opsz, sign);
        break;
      }
      case 0x9c:  // PUSHF
        Push(cpu, s, opsz, s->eflags | 2);
        break;
      case 0x9d: {  // POPF
        uint32_t v = Pop(cpu, s, opsz);
        s->eflags = opsz == 2 ? (s->eflags & 0xffff0000) | (v | 2) : v | 2;
        break;
      }
      case 0x9e:  // SAHF: SF ZF - AF - PF - CF from AH
        s->eflags = (s->eflags & ~0xd5u) | (RegRead(s, kRegAh, 1) & 0xd5u) | 2;
        break;
      case 0x9f:  // LAHF: low flags byte into AH
        RegWrite(s, kRegAh, 1, (s->eflags & 0xd5u) | 2);
        break;
      case 0xa0: case 0xa1: case 0xa2: case 0xa3: {  // MOV acc <-> moffs
        uint32_t off = addrsz == 2 ? Fetch16(&f) : Fetch32(&f);
        uint64_t lin = SegLinear(s, p.seg >= 0 ? p.seg : kSegDs) + off;
        if (opcode == 0xa0) {
          RegWrite(s, kEax, 1, BusRead(cpu->bus, lin, 1));
        } else if (opcode == 0xa1) {
          RegWrite(s, kEax, opsz, BusRead(cpu->bus, lin, opsz));
        } else if (opcode == 0xa2) {
          BusWrite(cpu->bus, lin, 1, RegRead(s, kEax, 1));
        } else {
          BusWrite(cpu->bus, lin, opsz, RegRead(s, kEax, opsz));
        }
        break;
      }
      case 0xa8: case 0xa9: {  // TEST acc, imm
        int size = opcode == 0xa9 ? opsz : 1;
        SetLogicFlags(s,
                      RegRead(s, kEax, size) & FetchImm(&f, size), size);
        break;
      }
      case 0xb0: case 0xb1: case 0xb2: case 0xb3:
      case 0xb4: case 0xb5: case 0xb6: case 0xb7:  // MOV r8, imm8
        RegWrite(s, opcode & 7, 1, Fetch8(&f));
        break;
      case 0xb8: case 0xb9: case 0xba: case 0xbb:
      case 0xbc: case 0xbd: case 0xbe: case 0xbf:  // MOV r, imm
        RegWrite(s, opcode & 7, opsz, FetchImm(&f, opsz));
        break;
      case 0xc0: case 0xc1: {  // GRP2 r/m, imm8
        DecodeModrm(&f, s, &p, addrsz, &m);
        int size = opcode == 0xc1 ? opsz : 1;
        uint32_t count = Fetch8(&f);
        RmWrite(cpu, s, &m, size,
                Shift(s, m.reg, RmRead(cpu, s, &m, size), count, size));
        break;
      }
      case 0xc2: {  // RET imm16
        uint32_t n = Fetch16(&f);
        f.ip = Pop(cpu, s, opsz);
        StackAdjust(s, opsz, (int)n);
        flow = kFlowRedirect;
        break;
      }
      case 0xc3:  // RET
        f.ip = Pop(cpu, s, opsz);
        flow = kFlowRedirect;
        break;
      case 0xc4: case 0xc5:  // LES / LDS r, m16:32
        DecodeModrm(&f, s, &p, addrsz, &m);
        if (!m.is_mem) goto illegal;
        LoadFar(cpu, s, &m, opsz, opcode == 0xc4 ? kSegEs : kSegDs);
        break;
      case 0xc6: case 0xc7: {  // MOV r/m, imm
        DecodeModrm(&f, s, &p, addrsz, &m);
        int size = opcode == 0xc7 ? opsz : 1;
        RmWrite(cpu, s, &m, size, FetchImm(&f, size));
        break;
      }
      case 0xc8: {  // ENTER imm16, imm8
        uint32_t size = Fetch16(&f);
        uint32_t level = Fetch8(&f) & 31;
        Push(cpu, s, opsz, RegRead(s, kEbp, opsz));
        uint32_t frame = StackOffset(s, opsz);
        if (level > 0) {
          for (uint32_t i = 1; i < level; i++) {
            RegWrite(s, kEbp, opsz, RegRead(s, kEbp, opsz) - opsz);
            StackSetSp(s, opsz, RegRead(s, kEbp, opsz));
            Push(cpu, s, opsz, Pop(cpu, s, opsz));  // Push([BP])
          }
          Push(cpu, s, opsz, frame);
        }
        RegWrite(s, kEbp, opsz, frame);
        StackAdjust(s, opsz, -(int)size);
        break;
      }
      case 0xc9: {  // LEAVE
        StackSetSp(s, opsz, RegRead(s, kEbp, opsz));
        RegWrite(s, kEbp, opsz, Pop(cpu, s, opsz));
        break;
      }
      case 0xca: {  // RETF imm16
        uint32_t n = Fetch16(&f);
        f.ip = Pop(cpu, s, opsz);
        LoadSegment(cpu, s, kSegCs, (uint16_t)Pop(cpu, s, opsz));
        StackAdjust(s, opsz, (int)n);
        flow = kFlowRedirect;
        break;
      }
      case 0xcb:  // RETF
        f.ip = Pop(cpu, s, opsz);
        LoadSegment(cpu, s, kSegCs, (uint16_t)Pop(cpu, s, opsz));
        flow = kFlowRedirect;
        break;
      case 0xcc:  // INT3
        DoInt(cpu, s, 3);
        flow = kFlowRedirect;
        break;
      case 0xcd:  // INT imm8
        DoInt(cpu, s, Fetch8(&f));
        flow = kFlowRedirect;
        break;
      case 0xce:  // INTO
        if (s->eflags & kFlagOf) {
          DoInt(cpu, s, 4);
          flow = kFlowRedirect;
        }
        break;
      case 0xcf: {  // IRET
        f.ip = Pop(cpu, s, opsz);
        uint32_t cs = Pop(cpu, s, opsz);
        uint32_t fl = Pop(cpu, s, opsz);
        LoadSegment(cpu, s, kSegCs, (uint16_t)cs);
        s->eflags = opsz == 2 ? (s->eflags & 0xffff0000) | (fl | 2) : fl | 2;
        flow = kFlowRedirect;
        break;
      }
      case 0xd0: case 0xd1: case 0xd2: case 0xd3: {  // GRP2 r/m, 1/CL
        DecodeModrm(&f, s, &p, addrsz, &m);
        int size = (opcode & 1) ? opsz : 1;
        uint32_t count = opcode <= 0xd1 ? 1 : RegRead(s, kEcx, 1);
        RmWrite(cpu, s, &m, size,
                Shift(s, m.reg, RmRead(cpu, s, &m, size), count, size));
        break;
      }
      case 0xd4: {  // AAM imm8
        uint32_t base = Fetch8(&f);
        if (base == 0) {
          DoInt(cpu, s, kVecDe);
          flow = kFlowRedirect;
          break;
        }
        uint32_t al = RegRead(s, kEax, 1);
        RegWrite(s, kEax + 4, 1, al / base);  // AH = AL / base
        RegWrite(s, kEax, 1, al % base);      // AL = AL mod base
        SetLogicFlags(s, al % base, 1);
        break;
      }
      case 0xd5: {  // AAD imm8
        uint32_t base = Fetch8(&f);
        uint32_t al =
            (RegRead(s, kEax, 1) + RegRead(s, kEax + 4, 1) * base) & 0xff;
        RegWrite(s, kEax, 1, al);
        RegWrite(s, kEax + 4, 1, 0);
        SetLogicFlags(s, al, 1);
        break;
      }
      case 0xd6:  // SALC (undocumented): AL = CF ? 0xff : 0x00
        RegWrite(s, kEax, 1, (s->eflags & kFlagCf) ? 0xff : 0x00);
        break;
      case 0xd7: {  // XLAT
        uint32_t off = addrsz == 2 ? RegRead(s, kEbx, 2) : RegRead(s, kEbx, 4);
        off += RegRead(s, kEax, 1);
        uint64_t lin = SegLinear(s, p.seg >= 0 ? p.seg : kSegDs) + off;
        RegWrite(s, kEax, 1, BusRead(cpu->bus, lin, 1));
        break;
      }
      case 0xd8: case 0xd9: case 0xda: case 0xdb:
      case 0xdc: case 0xdd: case 0xde: case 0xdf: {  // x87 ESC
        DecodeModrm(&f, s, &p, addrsz, &m);
        // Only FNINIT (DB /3) is accepted; this machine has no FPU.
        if (!(opcode == 0xdb && m.mod == 3 && m.reg == 3)) goto illegal;
        break;
      }
      case 0xe0: case 0xe1: case 0xe2: {  // LOOPNE / LOOPE / LOOP
        int8_t rel = FetchS8(&f);
        uint32_t count = RegRead(s, kEcx, opsz) - 1;
        RegWrite(s, kEcx, opsz, count);
        int taken = count != 0 &&
                    (opcode == 0xe2 || Cond(s, opcode == 0xe0 ? 5 : 4));
        if (taken) {
          f.ip = RelTarget(f.ip, rel, opsz);
          flow = kFlowRedirect;
        }
        break;
      }
      case 0xe3: {  // JCXZ / JECXZ (address-size register)
        int8_t rel = FetchS8(&f);
        if (RegRead(s, kEcx, addrsz) == 0) {
          f.ip = RelTarget(f.ip, rel, opsz);
          flow = kFlowRedirect;
        }
        break;
      }
      case 0xe4: {  // IN AL, imm8
        uint32_t port = Fetch8(&f);
        RegWrite(s, kEax, 1, BusRead(cpu->io, port, 1));
        break;
      }
      case 0xe5: {  // IN eAX, imm8
        uint32_t port = Fetch8(&f);
        RegWrite(s, kEax, opsz, BusRead(cpu->io, port, opsz));
        break;
      }
      case 0xe6:  // OUT imm8, AL
        BusWrite(cpu->io, Fetch8(&f), 1, RegRead(s, kEax, 1));
        break;
      case 0xe7:  // OUT imm8, eAX
        BusWrite(cpu->io, Fetch8(&f), opsz, RegRead(s, kEax, opsz));
        break;
      case 0xe8: {  // CALL rel
        int64_t rel = opsz == 2 ? FetchS16(&f) : FetchS32(&f);
        Push(cpu, s, opsz, opsz == 2 ? f.ip & 0xffff : f.ip);
        f.ip = RelTarget(f.ip, rel, opsz);
        flow = kFlowRedirect;
        break;
      }
      case 0xe9: {  // JMP rel
        int64_t rel = opsz == 2 ? FetchS16(&f) : FetchS32(&f);
        f.ip = RelTarget(f.ip, rel, opsz);
        flow = kFlowRedirect;
        break;
      }
      case 0xea: {  // JMP ptr16:16 / ptr16:32
        uint32_t off = opsz == 2 ? Fetch16(&f) : Fetch32(&f);
        uint16_t seg = Fetch16(&f);
        LoadSegment(cpu, s, kSegCs, seg);
        f.ip = off;
        flow = kFlowRedirect;
        break;
      }
      case 0xeb: {  // JMP rel8
        int8_t rel = FetchS8(&f);
        f.ip = RelTarget(f.ip, rel, opsz);
        flow = kFlowRedirect;
        break;
      }
      case 0xec:  // IN AL, DX
        RegWrite(s, kEax, 1, BusRead(cpu->io, RegRead(s, kEdx, 2), 1));
        break;
      case 0xed:  // IN eAX, DX
        RegWrite(s, kEax, opsz, BusRead(cpu->io, RegRead(s, kEdx, 2), opsz));
        break;
      case 0xee:  // OUT DX, AL
        BusWrite(cpu->io, RegRead(s, kEdx, 2), 1, RegRead(s, kEax, 1));
        break;
      case 0xef:  // OUT DX, eAX
        BusWrite(cpu->io, RegRead(s, kEdx, 2), opsz, RegRead(s, kEax, opsz));
        break;
      case 0xf4:  // HLT: no wake sources in this machine, so emulation ends
        LogInfo("hlt with no interrupt sources, stopping emulation");
        cpu->halted = kCpuExited;
        cpu->exit_code = 0;
        flow = kFlowRedirect;
        break;
      case 0xf5:  // CMC
        s->eflags ^= kFlagCf;
        break;
      case 0xf6: case 0xf7: {  // GRP3
        DecodeModrm(&f, s, &p, addrsz, &m);
        int size = opcode == 0xf7 ? opsz : 1;
        switch (m.reg) {
          case 0: case 1: {  // TEST r/m, imm
            uint32_t imm = FetchImm(&f, size);
            SetLogicFlags(s, RmRead(cpu, s, &m, size) & imm, size);
            break;
          }
          case 2:  // NOT
            RmWrite(cpu, s, &m, size, ~RmRead(cpu, s, &m, size));
            break;
          case 3: {  // NEG
            uint32_t v = RmRead(cpu, s, &m, size);
            uint32_t res = (-v) & ValueMask(size);
            SetSubFlags(s, 0, v, res, size, 0);
            RmWrite(cpu, s, &m, size, res);
            break;
          }
          default:  // 4-7: MUL/IMUL/DIV/IDIV
            if (ExecMulDiv(cpu, s, m.reg, size, RmRead(cpu, s, &m, size)))
              flow = kFlowRedirect;
            break;
        }
        break;
      }
      case 0xf8: s->eflags &= ~kFlagCf; break;  // CLC
      case 0xf9: s->eflags |= kFlagCf; break;   // STC
      case 0xfa: s->eflags &= ~kFlagIf; break;  // CLI
      case 0xfb: s->eflags |= kFlagIf; break;   // STI
      case 0xfc: s->eflags &= ~kFlagDf; break;  // CLD
      case 0xfd: s->eflags |= kFlagDf; break;   // STD
      case 0xfe: {  // GRP4: INC/DEC rm8
        DecodeModrm(&f, s, &p, addrsz, &m);
        if (m.reg > 1) goto illegal;
        uint32_t res = (RmRead(cpu, s, &m, 1) + (m.reg ? -1 : 1)) & 0xff;
        SetIncDecFlags(s, res, 1, m.reg == 0);
        RmWrite(cpu, s, &m, 1, res);
        break;
      }
      case 0xff: {  // GRP5
        DecodeModrm(&f, s, &p, addrsz, &m);
        switch (m.reg) {
          case 0: case 1: {  // INC/DEC r/m
            uint32_t res = (RmRead(cpu, s, &m, opsz) + (m.reg ? -1 : 1)) &
                           ValueMask(opsz);
            SetIncDecFlags(s, res, opsz, m.reg == 0);
            RmWrite(cpu, s, &m, opsz, res);
            break;
          }
          case 2: {  // CALL near r/m
            uint32_t target = RmRead(cpu, s, &m, opsz);
            Push(cpu, s, opsz, opsz == 2 ? f.ip & 0xffff : f.ip);
            f.ip = target;
            flow = kFlowRedirect;
            break;
          }
          case 3: {  // CALL far m16:32
            if (!m.is_mem) goto illegal;
            uint32_t target = RmRead(cpu, s, &m, opsz);
            uint16_t seg =
                (uint16_t)BusRead(cpu->bus, MemLinear(s, &m) + opsz, 2);
            Push(cpu, s, opsz, s->sreg[kSegCs]);
            Push(cpu, s, opsz, opsz == 2 ? f.ip & 0xffff : f.ip);
            LoadSegment(cpu, s, kSegCs, seg);
            f.ip = target;
            flow = kFlowRedirect;
            break;
          }
          case 4: {  // JMP near r/m
            f.ip = RmRead(cpu, s, &m, opsz);
            flow = kFlowRedirect;
            break;
          }
          case 5: {  // JMP far m16:32
            if (!m.is_mem) goto illegal;
            uint32_t target = RmRead(cpu, s, &m, opsz);
            uint16_t seg =
                (uint16_t)BusRead(cpu->bus, MemLinear(s, &m) + opsz, 2);
            LoadSegment(cpu, s, kSegCs, seg);
            f.ip = target;
            flow = kFlowRedirect;
            break;
          }
          case 6:  // PUSH r/m
            Push(cpu, s, opsz, RmRead(cpu, s, &m, opsz));
            break;
          default:
            goto illegal;
        }
        break;
      }
      default:
        goto illegal;
    }
  }

  if (flow == kFlowNext) s->eip = f.ip;
  return;

illegal:
  DoInt(cpu, s, kVecUd);  // s->eip still holds the faulting instruction
}
