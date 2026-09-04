// The x86 instruction set: fetch prefixes, decode modrm/SIB, execute — one C
// scope per instruction, one switch case per manual page, opcode order.
// Every name is the manual's name: eax/ax/al for the registers, cf/of for
// the flags, rm8/reg8/imm8 for the operands; the width lives in the name
// (rd8, rm16, push32) — never in a parameter. C operators do the arithmetic;
// helpers exist only for what C has no operator for (flags, stack, segments,
// interrupt dispatch). Scope: the 386 real-mode set plus the flat protected
// mode the multiboot contract enters (SDM); segment limits are not enforced
// — the descriptor cache surviving CR0.PE=0 IS big real mode.
//
// The step protocol around these switches (INTR sampling, prefix consumption,
// the single commit) lives in step.c.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus/bus.h"
#include "cpu/isa/isa.h"
#include "debug/debug.h"
#include "host/host.h"
#include "util/log.h"

#include "exec.h"  // last: it defines the short register macros x/f/eax/...

// The per-step view (cpu/s/fr/fl), the register and eip spellings, the decoder
// context `d` and the opcode switches' entry points' declarations all come
// from exec.h; these are the definitions.
CpuState* cpu;
x86_state* s;
frame* fr;
eflags* fl;

// ---- memory -----------------------------------------------------------------

// Open bus: nobody's address reads all ones and drops writes — real x86
// never faults there (SDM; QEMU's unassigned behavior agrees). All accesses
// funnel through bus_load/bus_store (the debug hub's observation points).
static uint64_t bus_load(uint64_t lin, int size) {
  BusRegion* r;
  if (BusProbe(cpu->bus, lin, size, &r) != 0) return size == 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
  uint64_t v = BusRead(cpu->bus, lin, size);
  if (DebugOn(kDbgBus) && !r->host) DebugBus(fr, r->ops->name, lin, size, 1, v);
  if (DebugOn(kDbgMem)) DebugMem(fr, lin, size, acc_read, v, 1);
  return v;
}

static void bus_store(uint64_t lin, int size, uint64_t v) {
  BusRegion* r;
  if (BusProbe(cpu->bus, lin, size, &r) != 0) return;
  if (DebugOn(kDbgBus) && !r->host) {
    DebugBus(fr, r->ops->name, lin, size, 0, v);
  } else if (DebugOn(kDbgMem)) {
    DebugMem(fr, lin, size, acc_write, v, 0);
  }
  BusWrite(cpu->bus, lin, size, v);
}

static uint8_t rd8(uint64_t lin) { return (uint8_t)bus_load(lin, 1); }
static uint16_t rd16(uint64_t lin) { return (uint16_t)bus_load(lin, 2); }
static uint32_t rd32(uint64_t lin) { return (uint32_t)bus_load(lin, 4); }
static void wr8(uint64_t lin, uint8_t v) { bus_store(lin, 1, v); }
static void wr16(uint64_t lin, uint16_t v) { bus_store(lin, 2, v); }
static void wr32(uint64_t lin, uint32_t v) { bus_store(lin, 4, v); }

// ---- flag rules (SDM "Flags Affected": one helper per family) ----------------

static uint32_t vmask(int size) { return size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1; }
static uint32_t vmsb(int size) { return 1u << (size * 8 - 1); }

// PF/ZF/SF from a result at a width (shared by every family).
static void szp(uint32_t r, int size) {
  int ones = 0;
  for (int i = 0; i < 8; i++) ones += (r >> i) & 1;
  fl->pf = !(ones & 1);
  fl->zf = (r & vmask(size)) == 0;
  fl->sf = (r & vmsb(size)) != 0;
}

void flags_add(uint32_t a, uint32_t b, uint32_t r, int size, int cin) {
  fl->cf = (uint64_t)(a & vmask(size)) + (b & vmask(size)) + cin > vmask(size);
  fl->af = ((a ^ b ^ r) >> 4) & 1;
  fl->of = (((~(a ^ b)) & (a ^ r)) >> (size * 8 - 1)) & 1;
  szp(r, size);
}

void flags_sub(uint32_t a, uint32_t b, uint32_t r, int size, int bin) {
  fl->cf = (uint64_t)(a & vmask(size)) < (uint64_t)(b & vmask(size)) + bin;
  fl->af = ((a ^ b ^ r) >> 4) & 1;
  fl->of = ((a ^ b) & (a ^ r) & vmsb(size)) != 0;
  szp(r, size);
}

void flags_logic(uint32_t r, int size) {
  fl->cf = 0;
  fl->of = 0;
  fl->af = 0;
  szp(r, size);
}

// INC/DEC keep CF (SDM).
void flags_inc(uint32_t r, int size) {
  fl->of = r == vmsb(size);
  fl->af = (r & 0xf) == 0;
  szp(r, size);
}

void flags_dec(uint32_t r, int size) {
  fl->of = r == vmsb(size) - 1;
  fl->af = (r & 0xf) == 0xf;
  szp(r, size);
}

// MUL/IMUL: CF=OF=1 when the high half is significant (SDM MUL/IMUL).
void flags_mul(uint64_t prod, int size) { fl->cf = fl->of = prod >> (size * 8) ? 1 : 0; }

void flags_imul(int64_t prod, int size) {
  int64_t low = (int64_t)((uint64_t)prod << (64 - size * 8)) >> (64 - size * 8);
  fl->cf = fl->of = prod != low;
}

// One decode context for the instruction being executed; the `dec` type and
// the `d` spelling live in exec.h (step.c shares them).
dec g;

// The instruction stream. fetch8 also records raw bytes for the trace.
uint8_t fetch8(void) {
  uint8_t v = rd8(s->base[cs_i] + eip + d.nxt);
  if (fr->rec.raw_len < sizeof(fr->rec.raw)) fr->rec.raw[fr->rec.raw_len++] = v;
  d.nxt++;
  return v;
}
static uint16_t fetch16(void) {
  uint16_t v = fetch8();
  return v | (uint16_t)fetch8() << 8;
}
static uint32_t fetch32(void) {
  uint32_t v = fetch16();
  return v | (uint32_t)fetch16() << 16;
}
static uint8_t imm8(void) { return fetch8(); }
static uint16_t imm16(void) { return fetch16(); }
static uint32_t imm32(void) { return fetch32(); }

// modrm/SIB (SDM vol.1 3.4.2-3.4.4). A BP/EBP base means SS, else DS; a
// segment override replaces the default for every reference.
static void modrm(void) {
  uint8_t enc = fetch8();
  d.mod = enc >> 6;
  d.reg = (enc >> 3) & 7;
  d.rm = enc & 7;
  d.mseg = d.seg;
  if (d.mod == 3) {
    d.is_mem = 0;
    return;
  }
  d.is_mem = 1;
  uint32_t off = 0;
  if (!d.a32) {
    // 16-bit addressing (table 2-1): base [+index] [+disp], [disp16].
    static const int base[8] = {ebx_i, ebx_i, ebp_i, ebp_i, esi_i, edi_i, ebp_i, ebx_i};
    static const int index[8] = {esi_i, edi_i, -1, -1, -1, -1, -1, -1};
    off = s->r[base[d.rm]].e;
    if (index[d.rm] >= 0) off += s->r[index[d.rm]].e;
    if (d.mod == 1)
      off += (int8_t)fetch8();
    else if (d.mod == 2)
      off += fetch16();
    else if (d.rm == 6)
      off = fetch16();  // [disp16]: no base register
    if (d.mseg < 0 && (d.rm == 2 || d.rm == 3 || (d.rm == 6 && d.mod != 0))) d.mseg = ss_i;
  } else if (d.rm == 4) {
    // SIB (table 2-3): scale*index + base (+ disp). Index 4 = no index;
    // base 5 with mod 0 = no base, disp32 instead — the index term applies
    // in every form.
    uint8_t sib = fetch8();
    int scale = 1 << (sib >> 6);
    int idx = (sib >> 3) & 7;
    int base = sib & 7;
    if (base == 5 && d.mod == 0) {
      off = fetch32();
    } else {
      off = s->r[base].e;
      if (d.mseg < 0 && base == 5) d.mseg = ss_i;
    }
    if (idx != 4) off += (uint32_t)scale * s->r[idx].e;
    if (d.mod == 1)
      off += (int8_t)fetch8();
    else if (d.mod == 2)
      off += fetch32();
  } else {
    if (d.rm == 5 && d.mod == 0) {
      off = fetch32();  // disp32, no base
    } else {
      off = s->r[d.rm].e;
      if (d.mod == 1)
        off += (int8_t)fetch8();
      else if (d.mod == 2)
        off += fetch32();
    }
    if (d.mseg < 0 && d.rm == 5 && d.mod != 0) d.mseg = ss_i;
  }
  if (d.mseg < 0) d.mseg = ds_i;
  d.moff = off;
  d.mlin = s->base[d.mseg] + off;
}

// ---- the rm/reg operands (SDM operand columns, one pair per width) ---------

// Byte registers 4..7 are the high halves of EAX..EBX (SDM 3.4.1).
static uint8_t reg8(void) { return d.reg < 4 ? s->r[d.reg].l : s->r[d.reg - 4].h; }
static void set_reg8(uint8_t v) {
  if (d.reg < 4)
    s->r[d.reg].l = v;
  else
    s->r[d.reg - 4].h = v;
}
static uint16_t reg16(void) { return s->r[d.reg].x; }
static void set_reg16(uint16_t v) { s->r[d.reg].x = v; }
static uint32_t reg32(void) { return s->r[d.reg].e; }
static void set_reg32(uint32_t v) { s->r[d.reg].e = v; }

static uint8_t rm8(void) {
  return d.is_mem ? rd8(d.mlin) : (d.rm < 4 ? s->r[d.rm].l : s->r[d.rm - 4].h);
}
static void set_rm8(uint8_t v) {
  if (d.is_mem)
    wr8(d.mlin, v);
  else if (d.rm < 4)
    s->r[d.rm].l = v;
  else
    s->r[d.rm - 4].h = v;
}
static uint16_t rm16(void) { return d.is_mem ? rd16(d.mlin) : s->r[d.rm].x; }
static void set_rm16(uint16_t v) {
  if (d.is_mem)
    wr16(d.mlin, v);
  else
    s->r[d.rm].x = v;
}
static uint32_t rm32(void) { return d.is_mem ? rd32(d.mlin) : s->r[d.rm].e; }
static void set_rm32(uint32_t v) {
  if (d.is_mem)
    wr32(d.mlin, v);
  else
    s->r[d.rm].e = v;
}

// The 8/16/32 rm access at the size the case wants, in one name each arm.
#define RM8() rm8()
#define SET_RM8(v) set_rm8(v)
#define RM16() rm16()
#define SET_RM16(v) set_rm16(v)
#define RM32() rm32()
#define SET_RM32(v) set_rm32(v)

// ---- the stack (SDM PUSH/POP; SP wraps at 64K in 16-bit stacks) -------------

static void push16(uint16_t v) {
  sp -= 2;
  wr16(s->base[ss_i] + sp, v);
}
static uint16_t pop16(void) {
  uint16_t v = rd16(s->base[ss_i] + sp);
  sp += 2;
  return v;
}
static void push32(uint32_t v) {
  esp -= 4;
  wr32(s->base[ss_i] + esp, v);
}
static uint32_t pop32(void) {
  uint32_t v = rd32(s->base[ss_i] + esp);
  esp += 4;
  return v;
}
// The operand-size polymorphic pair: `w32` picks the arm.
static void push_w(uint32_t v) {
  if (d.w32)
    push32(v);
  else
    push16((uint16_t)v);
}
static uint32_t pop_w(void) { return d.w32 ? pop32() : pop16(); }

// ---- segments and interrupts --------------------------------------------------

// Translation always uses the descriptor cache: protected-mode loads walk
// the GDT, real-mode loads set sel<<4. The cache SURVIVES CR0.PE=0 — that
// survival is big real mode (the kvm-unit-tests harness relies on it).
static void load_seg(int seg, uint16_t sel) {
  s->sreg[seg] = sel;
  if (!(s->cr0 & 1)) {
    s->base[seg] = (uint64_t)sel << 4;
    s->dbit[seg] = 0;
    return;
  }
  if (sel & 4) Fatal("x86: LDT selectors not implemented");
  uint64_t desc = BusRead(cpu->bus, s->gdtr + ((sel >> 3) & 0x1fff) * 8, 8);
  s->base[seg] = ((desc >> 16) & 0xffffff) | (((desc >> 56) & 0xff) << 24);
  s->dbit[seg] = (uint8_t)((desc >> 54) & 1);
}

// Delivers a vector through the IVT (IDTR base): read the entry, push
// flags/CS/return-IP, clear IF/TF, load CS:IP. Traps (software INT) push the
// next instruction, faults (#DE/#UD) push the faulting one (SDM 6-3).
void do_int(int vec, uint32_t ret_eip) {
  uint64_t tbl = s->idtr + (uint64_t)vec * 4;
  uint32_t off = rd16(tbl);
  uint32_t seg = rd16(tbl + 2);
  push16(fl->word | 2);
  push16(s->sreg[cs_i]);
  push16((uint16_t)ret_eip);
  fl->if_ = 0;
  fl->tf = 0;
  load_seg(cs_i, (uint16_t)seg);
  eip = off;
}

_Noreturn static void ud(void) { raise_(fr, vec_ud, (uint64_t)eip); }

// Divide errors (SDM DIV/IDIV): quotient overflow or zero divisor.
_Noreturn static void de(void) { raise_(fr, 0, (uint64_t)eip); }

// ---- conditions (SDM table Jcc; the low nibble is the condition) -----------

static int cond(int c) {
  int r;
  switch (c & ~1) {
    case 0x0:
      r = fl->of;
      break;
    case 0x2:
      r = fl->cf;
      break;
    case 0x4:
      r = fl->zf;
      break;
    case 0x6:
      r = fl->cf || fl->zf;
      break;
    case 0x8:
      r = fl->sf;
      break;
    case 0xa:
      r = fl->pf;
      break;
    case 0xc:
      r = fl->sf != fl->of;
      break;
    default:
      r = fl->zf || fl->sf != fl->of;
      break;  // 0xe
  }
  return r ^ (c & 1);
}

// ---- the shift/rotate family (SDM GRP2) --------------------------------------

static uint32_t rol(uint32_t v, uint32_t cnt, int size);
static uint32_t ror(uint32_t v, uint32_t cnt, int size);
static uint32_t rcl(uint32_t v, uint32_t cnt, int size);
static uint32_t rcr(uint32_t v, uint32_t cnt, int size);

static uint32_t shl(uint32_t v, uint32_t cnt, int size) {
  if (!cnt) return v;
  uint32_t width = (uint32_t)size * 8;
  uint32_t r = cnt >= width ? 0 : (v << cnt) & vmask(size);
  fl->cf = cnt < width && (v >> (width - cnt)) & 1;
  fl->of = ((r & vmsb(size)) != 0) ^ ((v & (vmsb(size) >> 1)) != 0);
  fl->af = 0;
  szp(r, size);
  return r;
}

static uint32_t shr(uint32_t v, uint32_t cnt, int size) {
  if (!cnt) return v;
  uint32_t width = (uint32_t)size * 8;
  uint32_t r = cnt >= width ? 0 : v >> cnt;
  fl->cf = (v >> (cnt - 1)) & 1;
  fl->of = cnt == 1 && (v & vmsb(size));
  fl->af = 0;
  szp(r, size);
  return r;
}

static uint32_t sar(uint32_t v, uint32_t cnt, int size) {
  if (!cnt) return v;
  uint32_t width = (uint32_t)size * 8;
  uint32_t fill = (v & vmsb(size)) ? vmask(size) : 0;
  uint32_t r = cnt >= width ? fill : ((v >> cnt) | (fill << (width - cnt))) & vmask(size);
  fl->cf = ((cnt >= width ? v : v >> (cnt - 1)) & vmsb(size)) != 0;
  fl->af = 0;
  szp(r, size);
  return r;
}

static uint32_t rol(uint32_t v, uint32_t cnt, int size) {
  if (!cnt) return v;
  uint32_t width = (uint32_t)size * 8;
  uint32_t k = cnt % width;
  uint32_t r = ((v << k) | (v >> (width - k))) & vmask(size);
  fl->cf = r & 1;
  fl->of = ((r & vmsb(size)) != 0) ^ ((r & (vmsb(size) >> 1)) != 0);
  return r;
}

static uint32_t ror(uint32_t v, uint32_t cnt, int size) {
  if (!cnt) return v;
  uint32_t width = (uint32_t)size * 8;
  uint32_t k = cnt % width;
  uint32_t r = ((v >> k) | (v << (width - k))) & vmask(size);
  fl->cf = (r & vmsb(size)) != 0;
  fl->of = ((r & vmsb(size)) != 0) ^ ((r & (vmsb(size) >> 1)) != 0);
  return r;
}

// Rotate through carry: a width+1 bit ring (SDM RCL/RCR).
static uint32_t rcl(uint32_t v, uint32_t cnt, int size) {
  if (!cnt) return v;
  uint32_t width = (uint32_t)size * 8;
  uint64_t ring = ((uint64_t)fl->cf << width) | (v & vmask(size));
  uint32_t k = cnt % (width + 1);
  ring = (ring << k) | (ring >> (width + 1 - k));
  fl->cf = (ring >> width) & 1;
  fl->of = (((ring & vmsb(size)) != 0) ^ (fl->cf != 0));
  return (uint32_t)ring & vmask(size);
}

static uint32_t rcr(uint32_t v, uint32_t cnt, int size) {
  if (!cnt) return v;
  uint32_t width = (uint32_t)size * 8;
  uint64_t ring = ((uint64_t)fl->cf << width) | (v & vmask(size));
  uint32_t k = cnt % (width + 1);
  ring = (ring >> k) | (ring << (width + 1 - k));
  fl->cf = (ring >> width) & 1;
  fl->of = (((ring & vmsb(size)) != 0) ^ (fl->cf != 0));
  return (uint32_t)ring & vmask(size);
}

// ---- GRP dispatch families (SDM 2-1 escape groups; reg-field picks)

// ---- GRP1 arithmetic (SDM 80..83): the reg field picks the operation --------
// kind: 0 add, 1 or, 2 adc, 3 sbb, 4 and, 5 sub, 6 xor, 7 cmp.

static uint32_t grp1_math(int kind, uint32_t a, uint32_t b, int size) {
  int carry = fl->cf;
  uint32_t r = 0;
  switch (kind) {
    case 0:
      r = (a + b) & vmask(size);
      flags_add(a, b, r, size, 0);
      break;
    case 2:
      r = (a + b + carry) & vmask(size);
      flags_add(a, b, r, size, carry);
      break;
    case 5:
    case 7:
      r = (a - b) & vmask(size);
      flags_sub(a, b, r, size, 0);
      break;
    case 3:
      r = (a - b - carry) & vmask(size);
      flags_sub(a, b, r, size, carry);
      break;
    case 1:
      r = a | b;
      flags_logic(r, size);
      break;
    case 4:
      r = a & b;
      flags_logic(r, size);
      break;
    default:
      r = a ^ b;
      flags_logic(r, size);
      break;  // 6: xor
  }
  return r;
}

static void grp1(int size, int imm_form) {
  // imm_form: 0 = imm at the width, 1 = imm8 sign-extended (83), 2 = imm8 (82)
  static const int kind[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  modrm();
  uint32_t src;
  if (imm_form == 1)
    src = (uint32_t)(int8_t)imm8();
  else if (imm_form == 2)
    src = imm8();
  else
    src = d.w32 ? imm32() : imm16();
  if (size == 1) {
    uint32_t a = RM8(), b = src & 0xff;
    uint32_t r = grp1_math(kind[d.reg], a, b, 1);
    if (d.reg != 7) SET_RM8(r);
  } else if (size == 2) {
    uint32_t a = RM16(), b = src & 0xffff;
    uint32_t r = grp1_math(kind[d.reg], a, b, 2);
    if (d.reg != 7) SET_RM16(r);
  } else {
    uint32_t a = RM32(), b = src;
    uint32_t r = grp1_math(kind[d.reg], a, b, 4);
    if (d.reg != 7) SET_RM32(r);
  }
}

// ---- GRP2 shifts/rotates (SDM c0/c1/d0..d3): reg field picks the op ---------

static void grp2(int size, uint32_t cnt) {
  modrm();  // the r/m operand: reg field selects the shift (table 2-2)
  cnt &= 31;
  uint32_t v, r;
  if (size == 1) {
    v = RM8();
    switch (d.reg) {
      case 0:
        r = rol(v, cnt, 1);
        break;
      case 1:
        r = ror(v, cnt, 1);
        break;
      case 2:
        r = rcl(v, cnt, 1);
        break;
      case 3:
        r = rcr(v, cnt, 1);
        break;
      case 4:
      case 6:
        r = shl(v, cnt, 1);
        break;
      case 5:
        r = shr(v, cnt, 1);
        break;
      default:
        r = sar(v, cnt, 1);
        break;
    }
    if (cnt) SET_RM8(r);  // count 0 changes nothing, not even the store path
  } else if (size == 2) {
    v = RM16();
    switch (d.reg) {
      case 0:
        r = rol(v, cnt, 2);
        break;
      case 1:
        r = ror(v, cnt, 2);
        break;
      case 2:
        r = rcl(v, cnt, 2);
        break;
      case 3:
        r = rcr(v, cnt, 2);
        break;
      case 4:
      case 6:
        r = shl(v, cnt, 2);
        break;
      case 5:
        r = shr(v, cnt, 2);
        break;
      default:
        r = sar(v, cnt, 2);
        break;
    }
    if (cnt) SET_RM16(r);
  } else {
    v = RM32();
    switch (d.reg) {
      case 0:
        r = rol(v, cnt, 4);
        break;
      case 1:
        r = ror(v, cnt, 4);
        break;
      case 2:
        r = rcl(v, cnt, 4);
        break;
      case 3:
        r = rcr(v, cnt, 4);
        break;
      case 4:
      case 6:
        r = shl(v, cnt, 4);
        break;
      case 5:
        r = shr(v, cnt, 4);
        break;
      default:
        r = sar(v, cnt, 4);
        break;
    }
    if (cnt) SET_RM32(r);
  }
}

// ---- multiply/divide (SDM GRP3 /4-/7) -----------------------------------------

// MUL r/m8: AX = AL * r/m8; the pair lives inside the EAX cell.
static void mul8(void) {
  uint32_t prod = (uint32_t)al * rm8();
  ax = (uint16_t)prod;
  flags_mul(prod, 1);
}

static void imul8(void) {
  int32_t prod = (int8_t)al * (int8_t)rm8();
  ax = (uint16_t)prod;
  flags_imul(prod, 1);
}

static void mul_w(uint32_t src) {
  uint64_t prod = (uint64_t)(d.w32 ? eax : ax) * src;
  if (d.w32) {
    eax = (uint32_t)prod;
    edx = (uint32_t)(prod >> 32);
  } else {
    ax = (uint16_t)prod;
    dx = (uint16_t)(prod >> 16);
  }
  flags_mul(prod, d.w32 ? 4 : 2);
}

static void imul_w(uint32_t src) {
  int bits = d.w32 ? 32 : 16;
  int64_t prod = ((int64_t)(d.w32 ? (int32_t)eax : (int16_t)ax)) *
                 (d.w32 ? (int32_t)src : (int16_t)(uint16_t)src);
  if (d.w32) {
    eax = (uint32_t)prod;
    edx = (uint32_t)((uint64_t)prod >> 32);
  } else {
    ax = (uint16_t)prod;
    dx = (uint16_t)((uint64_t)prod >> 16);
  }
  flags_imul(prod, bits / 8);
}

// DIV r/m8: AL = AX/src, AH = remainder; both halves of the EAX cell.
static void div8(void) {
  uint32_t dividend = ax;
  uint32_t divisor = rm8();
  if (divisor == 0 || dividend / divisor > 0xff) de();
  al = (uint8_t)(dividend / divisor);
  ah = (uint8_t)(dividend % divisor);
}

static void idiv8(void) {
  int32_t dividend = (int16_t)ax;
  int32_t divisor = (int8_t)rm8();
  if (divisor == 0) de();
  int32_t q = dividend / divisor;
  if (q != (int8_t)q) de();
  al = (uint8_t)q;
  ah = (uint8_t)(dividend % divisor);
}

static void div_w(uint32_t src) {
  uint32_t lo = d.w32 ? eax : ax;
  uint64_t dividend = ((uint64_t)(d.w32 ? edx : dx) << (d.w32 ? 32 : 16)) | lo;
  uint32_t divisor = src & vmask(d.w32 ? 4 : 2);
  if (divisor == 0 || dividend / divisor > vmask(d.w32 ? 4 : 2)) de();
  uint32_t q = (uint32_t)(dividend / divisor);
  uint32_t r = (uint32_t)(dividend % divisor);
  if (d.w32) {
    eax = q;
    edx = r;
  } else {
    ax = (uint16_t)q;
    dx = (uint16_t)r;
  }
}

static void idiv_w(uint32_t src) {
  int qbits = d.w32 ? 32 : 16;  // the quotient's width (SDM IDIV r/m32: #DE when
                                // the quotient is outside -2^31..2^31-1; r/m16 likewise)
  uint32_t lo = d.w32 ? eax : ax;
  int64_t dividend = (int64_t)((((uint64_t)(d.w32 ? edx : dx) << (d.w32 ? 32 : 16)) | lo) &
                               (d.w32 ? ~0ULL : 0xffffffffULL));
  if (!d.w32) dividend = (int32_t)(uint32_t)dividend;
  int64_t divisor = d.w32 ? (int32_t)src : (int16_t)(uint16_t)src;
  if (divisor == 0) de();
  int64_t q = dividend / divisor;
  int64_t hi = 1LL << (qbits - 1);  // 2^31 / 2^15, exact in int64 (no shift UB)
  if (q < -hi || q >= hi) de();
  int64_t r = dividend % divisor;
  if (d.w32) {
    eax = (uint32_t)q;
    edx = (uint32_t)r;
  } else {
    ax = (uint16_t)q;
    dx = (uint16_t)r;
  }
}

// ---- GRP3 (SDM f6/f7): test/not/neg/mul/imul/div/idiv ----------------------

static void grp3(int size) {
  modrm();
  if (d.reg <= 1) {  // test rm, imm
    uint32_t imm = size == 1 ? imm8() : d.w32 ? imm32() : imm16();
    uint32_t a = size == 1 ? RM8() : size == 2 ? RM16() : RM32();
    flags_logic(a & imm, size);
    return;
  }
  if (d.reg == 2) {  // not: no flags
    if (size == 1)
      SET_RM8(~RM8());
    else if (size == 2)
      SET_RM16(~RM16());
    else
      SET_RM32(~RM32());
    return;
  }
  if (d.reg == 3) {  // neg
    if (size == 1) {
      uint32_t v = RM8();
      uint8_t r = (uint8_t)-v;
      flags_sub(0, v, r, 1, 0);
      SET_RM8(r);
    } else if (size == 2) {
      uint32_t v = RM16();
      uint16_t r = (uint16_t)-v;
      flags_sub(0, v, r, 2, 0);
      SET_RM16(r);
    } else {
      uint32_t v = RM32();
      uint32_t r = -v;
      flags_sub(0, v, r, 4, 0);
      SET_RM32(r);
    }
    return;
  }
  if (size == 1) {
    switch (d.reg) {
      case 4:
        mul8();
        return;
      case 5:
        imul8();
        return;
      case 6:
        div8();
        return;
      default:
        idiv8();
        return;
    }
  }
  uint32_t src = size == 2 ? RM16() : RM32();
  switch (d.reg) {
    case 4:
      mul_w(src);
      return;
    case 5:
      imul_w(src);
      return;
    case 6:
      div_w(src);
      return;
    default:
      idiv_w(src);
      return;
  }
}

// ---- string operations (SDM MOVS..SCAS with REP/REPE/REPNE) -----------------
// SI uses DS (overridable); DI always ES. SI/DI advance by the address size,
// CX counts by the operand size. One iteration per step: a REP that wants
// another re-executes the instruction (interruptible, like hardware).
static int str_uses_si(int op) { return (op >= 0xa4 && op <= 0xa7) || op == 0xac || op == 0xad; }
static int str_uses_di(int op) {
  // MOVS/CMPS (a4-a7) and STOS/SCAS (aa-ab, ae-af); LODS (ac/ad) is SI only.
  return (op >= 0xa4 && op <= 0xa7) || op == 0xaa || op == 0xab || op == 0xae || op == 0xaf;
}

static void string_op(uint8_t op) {
  int size = (op & 1) && op != 0xa6 && op != 0xa7 && op != 0xae && op != 0xaf ? (d.w32 ? 4 : 2) : 1;
  int step = fl->df ? -size : size;
  uint32_t sio = d.a32 ? esi : (uint32_t)si;
  uint32_t dio = d.a32 ? edi : (uint32_t)di;
  uint64_t slin = s->base[d.seg >= 0 ? d.seg : ds_i] + sio;
  uint64_t dlin = s->base[es_i] + dio;
  switch (op) {
    case 0xa4:
    case 0xa5:  // movs
      if (size == 1)
        wr8(dlin, rd8(slin));
      else if (size == 2)
        wr16(dlin, rd16(slin));
      else
        wr32(dlin, rd32(slin));
      break;
    case 0xa6:
    case 0xa7: {  // cmps: [si] - [di]
      uint32_t a = size == 1 ? rd8(slin) : size == 2 ? rd16(slin) : rd32(slin);
      uint32_t b = size == 1 ? rd8(dlin) : size == 2 ? rd16(dlin) : rd32(dlin);
      uint32_t r = (a - b) & vmask(size);
      flags_sub(a, b, r, size, 0);
      break;
    }
    case 0xaa:
    case 0xab:  // stos
      if (size == 1)
        wr8(dlin, al);
      else if (size == 2)
        wr16(dlin, ax);
      else
        wr32(dlin, eax);
      break;
    case 0xac:
    case 0xad:  // lods
      if (size == 1)
        al = rd8(slin);
      else if (size == 2)
        ax = rd16(slin);
      else
        eax = rd32(slin);
      break;
    default: {  // 0xae/0xaf scas: eAX - [di]
      uint32_t a = size == 1 ? al : size == 2 ? ax : eax;
      uint32_t b = size == 1 ? rd8(dlin) : size == 2 ? rd16(dlin) : rd32(dlin);
      uint32_t r = (a - b) & vmask(size);
      flags_sub(a, b, r, size, 0);
      break;
    }
  }
  if (str_uses_si(op)) {
    if (d.a32)
      esi += (uint32_t)step;
    else
      si = (uint16_t)(sio + step);
  }
  if (str_uses_di(op)) {
    if (d.a32)
      edi += (uint32_t)step;
    else
      di = (uint16_t)(dio + step);
  }
  if (!d.rep) return;
  uint32_t cnt = (d.w32 ? ecx : cx) - 1;
  if (d.w32)
    ecx = cnt;
  else
    cx = (uint16_t)cnt;
  if (cnt == 0) return;
  // REPE/REPNE keep iterating while the comparison holds (SDM REP prefix).
  if ((op >= 0xa6 && op <= 0xa7) || op == 0xae || op == 0xaf)
    if (fl->zf != (d.rep == 1)) return;
  d.nxt = 0;  // REP continues: the step re-executes this instruction
}

// ---- BCD adjust (SDM DAA..AAD; SALC is the undocumented AL=CF<<8) -----------

static void daa(void) {
  uint8_t old_al = al;
  int old_cf = fl->cf;
  int c = 0;
  if ((al & 0xf) > 9 || fl->af) {
    al = (uint8_t)(old_al + 6);
    fl->af = 1;
    c = old_cf || old_al > 0xf9;
  } else {
    fl->af = 0;
  }
  if (old_al > 0x99 || old_cf) {  // original AL and original CF (tiny386 model)
    al += 0x60;
    c = 1;
  }
  // flags_logic clears AF; DAA/DAS keep the step-one AF (SDM "Flags Affected"
  // and tiny386 __DAA/__DAS_helper: only SF/ZF/PF are recomputed from AL).
  int keep_af = fl->af;
  flags_logic(al, 1);
  fl->af = keep_af;
  if (c) fl->cf = 1;
}

// DAS per the actual 386+ behavior, verified 0 fails against the
// kvm-unit-tests 1024-case truth table (dascheck offline harness): both
// conditions test the ORIGINAL AL and ORIGINAL CF (tiny386 model; SDM's
// step-two re-read of the decremented AL diverges on 26 cases). AF is the
// step-one value, never cleared.
static void das(void) {
  uint8_t old_al = al;
  int old_cf = fl->cf;
  int c = 0;
  if ((al & 0xf) > 9 || fl->af) {
    al = (uint8_t)(old_al - 6);
    fl->af = 1;
    c = old_cf || old_al < 6;
  } else {
    fl->af = 0;
  }
  if (old_al > 0x99 || old_cf) {  // original AL and original CF
    al -= 0x60;
    c = 1;
  }
  int keep_af = fl->af;
  flags_logic(al, 1);
  fl->af = keep_af;
  if (c) fl->cf = 1;
}

static void aaa(void) {
  if ((al & 0xf) > 9 || fl->af) {
    ax += 0x106;
    fl->af = 1;
    fl->cf = 1;
  } else {
    fl->af = 0;
    fl->cf = 0;
  }
  al &= 0xf;
}

static void aas(void) {
  if ((al & 0xf) > 9 || fl->af) {
    ax -= 6;
    ah -= 1;
    fl->af = 1;
    fl->cf = 1;
  } else {
    fl->af = 0;
    fl->cf = 0;
  }
  al &= 0xf;
}

// ---- the instruction switch ---------------------------------------------------
// One case per manual page, opcode order (SDM vol.2 table A-2); the comment
// is the manual's page title. The ALU family (00..3d) keeps the manual's own
// six forms per operation — rm8,r8 / rm,r / r8,rm8 / r,rm / al,imm8 / eAX,imm —
// so each case reads exactly like its page.

// The 0f two-byte dispatch.
static void run_op2(uint8_t op2) {
  switch (op2) {
    case 0x01: {  // grp6: sgdt/sidt/lgdt/lidt/smsw (reg field)
      modrm();
      switch (d.reg) {
        case 0:
        case 1: {  // sgdt/sidt: store limit(2) then base(4)
          uint64_t base = d.reg == 0 ? s->gdtr : s->idtr;
          uint16_t limit = d.reg == 0 ? s->gdtr_limit : s->idtr_limit;
          wr16(d.mlin, limit);
          wr32(d.mlin + 2, (uint32_t)base);
          break;
        }
        case 2:
        case 3: {  // lgdt/lidt
          uint16_t limit = rd16(d.mlin);
          uint64_t base = rd32(d.mlin + 2);
          if (d.reg == 2) {
            s->gdtr = base;
            s->gdtr_limit = limit;
          } else {
            s->idtr = base;
            s->idtr_limit = limit;
          }
          break;
        }
        case 4:  // smsw: CR0 at the rm width
          if (d.is_mem) {
            if (d.w32)
              wr32(d.mlin, s->cr0);
            else
              wr16(d.mlin, (uint16_t)s->cr0);
          } else {
            if (d.w32)
              set_rm32(s->cr0);
            else
              set_rm16((uint16_t)s->cr0);
          }
          break;
        default:
          ud();  // lmsw/invlpg: stage-4 scope
      }
      break;
    }
    case 0x0b:
      ud();
      break;  // ud2
    case 0x1f:
      modrm();
      break;    // multi-byte nop
    case 0x20:  // mov r32, cr0
      modrm();
      if (d.reg != 0) ud();
      s->r[d.rm].e = s->cr0;
      break;
    case 0x22: {  // mov cr0, r32 (PE drives the mode switches)
      modrm();
      if (d.reg != 0) ud();
      s->cr0 = s->r[d.rm].e;
      break;
    }
    case 0x21:  // mov r32, drn
      modrm();
      s->r[d.rm].e = s->dr[d.reg];
      break;
    case 0x23:  // mov drn, r32
      modrm();
      s->dr[d.reg] = s->r[d.rm].e;
      break;
    case 0x80:
    case 0x81:
    case 0x82:
    case 0x83:
    case 0x84:
    case 0x85:
    case 0x86:
    case 0x87:
    case 0x88:
    case 0x89:
    case 0x8a:
    case 0x8b:
    case 0x8c:
    case 0x8d:
    case 0x8e:
    case 0x8f: {  // jcc rel16/32
      int32_t rel = d.w32 ? (int32_t)fetch32() : (int16_t)fetch16();
      if (cond(op2 & 0xf)) d.nxt += (uint32_t)rel;
      break;
    }
    case 0x90:
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97:
    case 0x98:
    case 0x99:
    case 0x9a:
    case 0x9b:
    case 0x9c:
    case 0x9d:
    case 0x9e:
    case 0x9f: {  // setcc rm8
      modrm();
      SET_RM8(cond(op2 & 0xf));
      break;
    }
    case 0xa0:
      push_w(s->sreg[fs_i]);
      break;  // push fs
    case 0xa1:
      load_seg(fs_i, (uint16_t)pop_w());
      break;      // pop fs
    case 0xa2: {  // cpuid
      uint32_t leaf = eax;
      if (leaf == 0) {
        eax = 1;
        ebx = 0x756e6547;
        edx = 0x49656e69;
        ecx = 0x6c65746e;
      } else if (leaf == 1) {
        // Family 6, model 3, stepping 3; no FPU, no TSC.
        eax = 0x633;
        ebx = ecx = edx = 0;
      } else {
        eax = ebx = ecx = edx = 0;
      }
      break;
    }
    case 0xa3:
    case 0xab:
    case 0xb3:
    case 0xbb: {  // bt/bts/btr/btc rm, reg
      modrm();
      uint32_t v, bit, pos;
      if (d.w32) {
        v = RM32();
        bit = reg32();
        pos = bit & 31;
      } else {
        v = RM16();
        bit = reg16();
        pos = bit & 15;
      }
      fl->cf = (v >> pos) & 1;
      if (op2 == 0xab)
        v |= 1u << pos;
      else if (op2 == 0xb3)
        v &= ~(1u << pos);
      else if (op2 == 0xbb)
        v ^= 1u << pos;
      if (op2 != 0xa3) {
        if (d.w32)
          SET_RM32(v);
        else
          SET_RM16(v);
      }
      break;
    }
    case 0xa4:
    case 0xa5: {  // shld rm, reg, imm8/cl
      modrm();
      uint32_t cnt = (op2 == 0xa4 ? imm8() : cl) & 31;
      if (d.w32) {
        uint32_t dst = RM32(), src = reg32();
        uint64_t comb = ((uint64_t)dst << 32) | src;
        uint32_t r = (uint32_t)(comb << cnt >> 32);
        if (cnt) {
          fl->cf = (comb >> (64 - cnt)) & 1;
          fl->of = ((r & 0x80000000u) != 0) ^ ((dst & 0x40000000u) != 0);
          szp(r, 4);
        }
        SET_RM32(r);
      } else {
        uint16_t dst = RM16(), src = reg16();
        uint32_t comb = ((uint32_t)dst << 16) | src;
        uint16_t r = (uint16_t)(comb << cnt >> 16);
        if (cnt) {
          fl->cf = (comb >> (32 - cnt)) & 1;
          szp(r, 2);
        }
        SET_RM16(r);
      }
      break;
    }
    case 0xac:
    case 0xad: {  // shrd rm, reg, imm8/cl
      modrm();
      uint32_t cnt = (op2 == 0xac ? imm8() : cl) & 31;
      if (d.w32) {
        uint32_t dst = RM32(), src = reg32();
        uint64_t comb = ((uint64_t)src << 32) | dst;
        uint32_t r = (uint32_t)(comb >> cnt);
        if (cnt) {
          fl->cf = (comb >> (cnt - 1)) & 1;
          szp(r, 4);
        }
        SET_RM32(r);
      } else {
        uint16_t dst = RM16(), src = reg16();
        uint32_t comb = ((uint32_t)src << 16) | dst;
        uint16_t r = (uint16_t)(comb >> cnt);
        if (cnt) {
          fl->cf = (comb >> (cnt - 1)) & 1;
          szp(r, 2);
        }
        SET_RM16(r);
      }
      break;
    }
    case 0xaf: {  // imul r, rm
      modrm();
      if (d.w32) {
        int64_t prod = (int64_t)(int32_t)RM32() * (int32_t)reg32();
        set_reg32((uint32_t)prod);
        flags_imul(prod, 4);
      } else {
        int32_t prod = (int32_t)(int16_t)RM16() * (int16_t)reg16();
        set_reg16((uint16_t)prod);
        flags_imul(prod, 2);
      }
      break;
    }
    case 0xb2:
    case 0xb4:
    case 0xb5: {  // lss/lfs/lgs r, m16:16/32
      modrm();
      if (!d.is_mem) ud();
      int seg = op2 == 0xb2 ? ss_i : op2 == 0xb4 ? fs_i : gs_i;
      if (d.w32) {
        set_reg32(RM32());
        load_seg(seg, rd16(d.mlin + 4));
      } else {
        set_reg16(RM16());
        load_seg(seg, rd16(d.mlin + 2));
      }
      break;
    }
    case 0xb6: {  // movzx r16/32, rm8
      modrm();
      if (d.w32)
        set_reg32(rm8());
      else
        set_reg16(rm8());
      break;
    }
    case 0xb7:  // movzx r32, rm16
      modrm();
      set_reg32(rm16());
      break;
    case 0xba: {  // grp8: bt/bts/btr/btc rm, imm8 (reg field 4-7)
      modrm();
      if (d.reg < 4) ud();
      uint32_t v, pos = imm8();
      if (d.w32) {
        v = RM32();
        pos &= 31;
      } else {
        v = RM16();
        pos &= 15;
      }
      fl->cf = (v >> pos) & 1;
      if (d.reg == 5)
        v |= 1u << pos;
      else if (d.reg == 6)
        v &= ~(1u << pos);
      else if (d.reg == 7)
        v ^= 1u << pos;
      if (d.w32)
        SET_RM32(v);
      else
        SET_RM16(v);
      break;
    }
    case 0xbe: {  // movsx r16/32, rm8
      modrm();
      uint32_t v = (uint32_t)(int8_t)rm8();
      if (d.w32)
        set_reg32(v);
      else
        set_reg16((uint16_t)v);
      break;
    }
    case 0xbf:  // movsx r32, rm16
      modrm();
      set_reg32((uint32_t)(int32_t)(int16_t)rm16());
      break;
    case 0xc0:
    case 0xc1: {  // xadd rm, reg
      modrm();
      if (op2 & 1) {
        if (d.w32) {
          uint32_t dst = RM32(), src = reg32();
          uint32_t r = dst + src;
          SET_RM32(r);
          set_reg32(dst);
          flags_add(dst, src, r, 4, 0);
        } else {
          uint16_t dst = RM16(), src = reg16();
          uint16_t r = (uint16_t)(dst + src);
          SET_RM16(r);
          set_reg16(dst);
          flags_add(dst, src, r, 2, 0);
        }
      } else {
        uint8_t dst = RM8(), src = reg8();
        uint8_t r = (uint8_t)(dst + src);
        SET_RM8(r);
        set_reg8(dst);
        flags_add(dst, src, r, 1, 0);
      }
      break;
    }
    case 0xc8:
    case 0xc9:
    case 0xca:
    case 0xcb:
    case 0xcc:
    case 0xcd:
    case 0xce:
    case 0xcf: {  // bswap r32
      uint32_t v = s->r[op2 & 7].e;
      s->r[op2 & 7].e = ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
      break;
    }
    default:
      ud();
  }
}

// The one-byte opcodes, manual order. The comment names the encoding; body
// is the manual's Operation section.
void run_op(uint8_t op) {
  if (op == 0x0f) {
    run_op2(fetch8());
    return;
  }  // two-byte escape (SDM 0f)
  switch (op) {
    case 0x00: {
      modrm();
      uint8_t a = RM8(), b = reg8();
      uint8_t r = a + b;
      SET_RM8(r);
      flags_add(a, b, r, 1, 0);
      break;
    }  // add rm8, r8
    case 0x01: {
      modrm();
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        uint32_t r = a + b;
        SET_RM32(r);
        flags_add(a, b, r, 4, 0);
      } else {
        uint16_t a = RM16(), b = reg16();
        uint16_t r = a + b;
        SET_RM16(r);
        flags_add(a, b, r, 2, 0);
      }
      break;
    }  // add rm, r
    case 0x02: {
      modrm();
      uint8_t a = reg8(), b = RM8();
      uint8_t r = a + b;
      set_reg8(r);
      flags_add(a, b, r, 1, 0);
      break;
    }  // add r8, rm8
    case 0x03: {
      modrm();
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        uint32_t r = a + b;
        set_reg32(r);
        flags_add(a, b, r, 4, 0);
      } else {
        uint16_t a = reg16(), b = RM16();
        uint16_t r = a + b;
        set_reg16(r);
        flags_add(a, b, r, 2, 0);
      }
      break;
    }  // add r, rm
    case 0x04: {
      uint8_t a = al, b = imm8();
      al = a + b;
      flags_add(a, b, al, 1, 0);
      break;
    }  // add al, imm8
    case 0x05: {
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        eax = a + b;
        flags_add(a, b, eax, 4, 0);
      } else {
        uint16_t a = ax, b = imm16();
        ax = a + b;
        flags_add(a, b, ax, 2, 0);
      }
      break;
    }  // add eAX, imm
    case 0x06:
      push_w(s->sreg[es_i]);
      break;  // push es
    case 0x07:
      load_seg(es_i, (uint16_t)pop_w());
      break;  // pop es
    case 0x08: {
      modrm();
      uint8_t a = RM8(), b = reg8();
      uint8_t r = a | b;
      SET_RM8(r);
      flags_logic(r, 1);
      break;
    }  // or rm8, r8
    case 0x09: {
      modrm();
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        uint32_t r = a | b;
        SET_RM32(r);
        flags_logic(r, 4);
      } else {
        uint16_t a = RM16(), b = reg16();
        uint16_t r = a | b;
        SET_RM16(r);
        flags_logic(r, 2);
      }
      break;
    }  // or rm, r
    case 0x0a: {
      modrm();
      uint8_t a = reg8(), b = RM8();
      uint8_t r = a | b;
      set_reg8(r);
      flags_logic(r, 1);
      break;
    }  // or r8, rm8
    case 0x0b: {
      modrm();
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        uint32_t r = a | b;
        set_reg32(r);
        flags_logic(r, 4);
      } else {
        uint16_t a = reg16(), b = RM16();
        uint16_t r = a | b;
        set_reg16(r);
        flags_logic(r, 2);
      }
      break;
    }  // or r, rm
    case 0x0c: {
      uint8_t a = al, b = imm8();
      al = a | b;
      flags_logic(al, 1);
      break;
    }  // or al, imm8
    case 0x0d: {
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        eax = a | b;
        flags_logic(eax, 4);
      } else {
        uint16_t a = ax, b = imm16();
        ax = a | b;
        flags_logic(ax, 2);
      }
      break;
    }  // or eAX, imm
    case 0x0e:
      push_w(s->sreg[cs_i]);
      break;  // push cs
    case 0x10: {
      modrm();
      uint8_t a = RM8(), b = reg8();
      int cin = fl->cf;
      uint8_t r = (uint8_t)(a + b + cin);
      SET_RM8(r);
      flags_add(a, b, r, 1, cin);
      break;
    }  // adc rm8, r8
    case 0x11: {
      modrm();
      int cin = fl->cf;
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        uint32_t r = a + b + cin;
        SET_RM32(r);
        flags_add(a, b, r, 4, cin);
      } else {
        uint16_t a = RM16(), b = reg16();
        uint16_t r = (uint16_t)(a + b + cin);
        SET_RM16(r);
        flags_add(a, b, r, 2, cin);
      }
      break;
    }  // adc rm, r
    case 0x12: {
      modrm();
      uint8_t a = reg8(), b = RM8();
      int cin = fl->cf;
      uint8_t r = (uint8_t)(a + b + cin);
      set_reg8(r);
      flags_add(a, b, r, 1, cin);
      break;
    }  // adc r8, rm8
    case 0x13: {
      modrm();
      int cin = fl->cf;
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        uint32_t r = a + b + cin;
        set_reg32(r);
        flags_add(a, b, r, 4, cin);
      } else {
        uint16_t a = reg16(), b = RM16();
        uint16_t r = (uint16_t)(a + b + cin);
        set_reg16(r);
        flags_add(a, b, r, 2, cin);
      }
      break;
    }  // adc r, rm
    case 0x14: {
      uint8_t a = al, b = imm8();
      int cin = fl->cf;
      al = (uint8_t)(a + b + cin);
      flags_add(a, b, al, 1, cin);
      break;
    }  // adc al, imm8
    case 0x15: {
      int cin = fl->cf;
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        eax = a + b + cin;
        flags_add(a, b, eax, 4, cin);
      } else {
        uint16_t a = ax, b = imm16();
        ax = (uint16_t)(a + b + cin);
        flags_add(a, b, ax, 2, cin);
      }
      break;
    }  // adc eAX, imm
    case 0x16:
      push_w(s->sreg[ss_i]);
      break;  // push ss
    case 0x17:
      load_seg(ss_i, (uint16_t)pop_w());
      break;  // pop ss
    case 0x18: {
      modrm();
      uint8_t a = RM8(), b = reg8();
      int cin = fl->cf;
      uint8_t r = (uint8_t)(a - b - cin);
      SET_RM8(r);
      flags_sub(a, b, r, 1, cin);
      break;
    }  // sbb rm8, r8
    case 0x19: {
      modrm();
      int cin = fl->cf;
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        uint32_t r = a - b - cin;
        SET_RM32(r);
        flags_sub(a, b, r, 4, cin);
      } else {
        uint16_t a = RM16(), b = reg16();
        uint16_t r = (uint16_t)(a - b - cin);
        SET_RM16(r);
        flags_sub(a, b, r, 2, cin);
      }
      break;
    }  // sbb rm, r
    case 0x1a: {
      modrm();
      uint8_t a = reg8(), b = RM8();
      int cin = fl->cf;
      uint8_t r = (uint8_t)(a - b - cin);
      set_reg8(r);
      flags_sub(a, b, r, 1, cin);
      break;
    }  // sbb r8, rm8
    case 0x1b: {
      modrm();
      int cin = fl->cf;
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        uint32_t r = a - b - cin;
        set_reg32(r);
        flags_sub(a, b, r, 4, cin);
      } else {
        uint16_t a = reg16(), b = RM16();
        uint16_t r = (uint16_t)(a - b - cin);
        set_reg16(r);
        flags_sub(a, b, r, 2, cin);
      }
      break;
    }  // sbb r, rm
    case 0x1c: {
      uint8_t a = al, b = imm8();
      int cin = fl->cf;
      al = (uint8_t)(a - b - cin);
      flags_sub(a, b, al, 1, cin);
      break;
    }  // sbb al, imm8
    case 0x1d: {
      int cin = fl->cf;
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        eax = a - b - cin;
        flags_sub(a, b, eax, 4, cin);
      } else {
        uint16_t a = ax, b = imm16();
        ax = (uint16_t)(a - b - cin);
        flags_sub(a, b, ax, 2, cin);
      }
      break;
    }  // sbb eAX, imm
    case 0x1e:
      push_w(s->sreg[ds_i]);
      break;  // push ds
    case 0x1f:
      load_seg(ds_i, (uint16_t)pop_w());
      break;  // pop ds
    case 0x20: {
      modrm();
      uint8_t a = RM8(), b = reg8();
      uint8_t r = a & b;
      SET_RM8(r);
      flags_logic(r, 1);
      break;
    }  // and rm8, r8
    case 0x21: {
      modrm();
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        uint32_t r = a & b;
        SET_RM32(r);
        flags_logic(r, 4);
      } else {
        uint16_t a = RM16(), b = reg16();
        uint16_t r = a & b;
        SET_RM16(r);
        flags_logic(r, 2);
      }
      break;
    }  // and rm, r
    case 0x22: {
      modrm();
      uint8_t a = reg8(), b = RM8();
      uint8_t r = a & b;
      set_reg8(r);
      flags_logic(r, 1);
      break;
    }  // and r8, rm8
    case 0x23: {
      modrm();
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        uint32_t r = a & b;
        set_reg32(r);
        flags_logic(r, 4);
      } else {
        uint16_t a = reg16(), b = RM16();
        uint16_t r = a & b;
        set_reg16(r);
        flags_logic(r, 2);
      }
      break;
    }  // and r, rm
    case 0x24: {
      uint8_t a = al, b = imm8();
      al = a & b;
      flags_logic(al, 1);
      break;
    }  // and al, imm8
    case 0x25: {
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        eax = a & b;
        flags_logic(eax, 4);
      } else {
        uint16_t a = ax, b = imm16();
        ax = a & b;
        flags_logic(ax, 2);
      }
      break;
    }  // and eAX, imm
    case 0x27:
      daa();
      break;  // daa
    case 0x28: {
      modrm();
      uint8_t a = RM8(), b = reg8();
      uint8_t r = a - b;
      SET_RM8(r);
      flags_sub(a, b, r, 1, 0);
      break;
    }  // sub rm8, r8
    case 0x29: {
      modrm();
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        uint32_t r = a - b;
        SET_RM32(r);
        flags_sub(a, b, r, 4, 0);
      } else {
        uint16_t a = RM16(), b = reg16();
        uint16_t r = (uint16_t)(a - b);
        SET_RM16(r);
        flags_sub(a, b, r, 2, 0);
      }
      break;
    }  // sub rm, r
    case 0x2a: {
      modrm();
      uint8_t a = reg8(), b = RM8();
      uint8_t r = a - b;
      set_reg8(r);
      flags_sub(a, b, r, 1, 0);
      break;
    }  // sub r8, rm8
    case 0x2b: {
      modrm();
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        uint32_t r = a - b;
        set_reg32(r);
        flags_sub(a, b, r, 4, 0);
      } else {
        uint16_t a = reg16(), b = RM16();
        uint16_t r = (uint16_t)(a - b);
        set_reg16(r);
        flags_sub(a, b, r, 2, 0);
      }
      break;
    }  // sub r, rm
    case 0x2c: {
      uint8_t a = al, b = imm8();
      al = (uint8_t)(a - b);
      flags_sub(a, b, al, 1, 0);
      break;
    }  // sub al, imm8
    case 0x2d: {
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        eax = a - b;
        flags_sub(a, b, eax, 4, 0);
      } else {
        uint16_t a = ax, b = imm16();
        ax = (uint16_t)(a - b);
        flags_sub(a, b, ax, 2, 0);
      }
      break;
    }  // sub eAX, imm
    case 0x2f:
      das();
      break;  // das
    case 0x30: {
      modrm();
      uint8_t a = RM8(), b = reg8();
      uint8_t r = a ^ b;
      SET_RM8(r);
      flags_logic(r, 1);
      break;
    }  // xor rm8, r8
    case 0x31: {
      modrm();
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        uint32_t r = a ^ b;
        SET_RM32(r);
        flags_logic(r, 4);
      } else {
        uint16_t a = RM16(), b = reg16();
        uint16_t r = a ^ b;
        SET_RM16(r);
        flags_logic(r, 2);
      }
      break;
    }  // xor rm, r
    case 0x32: {
      modrm();
      uint8_t a = reg8(), b = RM8();
      uint8_t r = a ^ b;
      set_reg8(r);
      flags_logic(r, 1);
      break;
    }  // xor r8, rm8
    case 0x33: {
      modrm();
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        uint32_t r = a ^ b;
        set_reg32(r);
        flags_logic(r, 4);
      } else {
        uint16_t a = reg16(), b = RM16();
        uint16_t r = a ^ b;
        set_reg16(r);
        flags_logic(r, 2);
      }
      break;
    }  // xor r, rm
    case 0x34: {
      uint8_t a = al, b = imm8();
      al = a ^ b;
      flags_logic(al, 1);
      break;
    }  // xor al, imm8
    case 0x35: {
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        eax = a ^ b;
        flags_logic(eax, 4);
      } else {
        uint16_t a = ax, b = imm16();
        ax = a ^ b;
        flags_logic(ax, 2);
      }
      break;
    }  // xor eAX, imm
    case 0x37:
      aaa();
      break;  // aaa
    case 0x38: {
      modrm();
      uint8_t a = RM8(), b = reg8();
      flags_sub(a, b, (uint8_t)(a - b), 1, 0);
      break;
    }  // cmp rm8, r8
    case 0x39: {
      modrm();
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        flags_sub(a, b, a - b, 4, 0);
      } else {
        uint16_t a = RM16(), b = reg16();
        flags_sub(a, b, (uint16_t)(a - b), 2, 0);
      }
      break;
    }  // cmp rm, r
    case 0x3a: {
      modrm();
      uint8_t a = reg8(), b = RM8();
      flags_sub(a, b, (uint8_t)(a - b), 1, 0);
      break;
    }  // cmp r8, rm8
    case 0x3b: {
      modrm();
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        flags_sub(a, b, a - b, 4, 0);
      } else {
        uint16_t a = reg16(), b = RM16();
        flags_sub(a, b, (uint16_t)(a - b), 2, 0);
      }
      break;
    }  // cmp r, rm
    case 0x3c: {
      uint8_t a = al, b = imm8();
      flags_sub(a, b, (uint8_t)(a - b), 1, 0);
      break;
    }  // cmp al, imm8
    case 0x3d: {
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        flags_sub(a, b, a - b, 4, 0);
      } else {
        uint16_t a = ax, b = imm16();
        flags_sub(a, b, (uint16_t)(a - b), 2, 0);
      }
      break;
    }  // cmp eAX, imm
    case 0x3f:
      aas();
      break;  // aas
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47: {  // inc r
      if (d.w32) {
        uint32_t r = s->r[op & 7].e + 1;
        s->r[op & 7].e = r;
        flags_inc(r, 4);
      } else {
        uint16_t r = (uint16_t)(s->r[op & 7].x + 1);
        s->r[op & 7].x = r;
        flags_inc(r, 2);
      }
      break;
    }
    case 0x48:
    case 0x49:
    case 0x4a:
    case 0x4b:
    case 0x4c:
    case 0x4d:
    case 0x4e:
    case 0x4f: {  // dec r
      if (d.w32) {
        uint32_t r = s->r[op & 7].e - 1;
        s->r[op & 7].e = r;
        flags_dec(r, 4);
      } else {
        uint16_t r = (uint16_t)(s->r[op & 7].x - 1);
        s->r[op & 7].x = r;
        flags_dec(r, 2);
      }
      break;
    }
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:  // push r
      push_w(d.w32 ? s->r[op & 7].e : s->r[op & 7].x);
      break;
    case 0x58:
    case 0x59:
    case 0x5a:
    case 0x5b:
    case 0x5c:
    case 0x5d:
    case 0x5e:
    case 0x5f:  // pop r
      if (d.w32)
        s->r[op & 7].e = pop32();
      else
        s->r[op & 7].x = pop16();
      break;
    case 0x60: {  // pusha: ax cx dx bx sp bp si di, sp = the ORIGINAL value
      uint32_t sp0 = d.w32 ? esp : sp;
      if (d.w32) {
        push32(eax);
        push32(ecx);
        push32(edx);
        push32(ebx);
        push32(sp0);
        push32(ebp);
        push32(esi);
        push32(edi);
      } else {
        push16(ax);
        push16(cx);
        push16(dx);
        push16(bx);
        push16((uint16_t)sp0);
        push16(bp);
        push16(si);
        push16(di);
      }
      break;
    }
    case 0x61: {  // popa: di si bp (skip sp) bx dx cx ax
      if (d.w32) {
        edi = pop32();
        esi = pop32();
        ebp = pop32();
        (void)pop32();
        ebx = pop32();
        edx = pop32();
        ecx = pop32();
        eax = pop32();
      } else {
        di = pop16();
        si = pop16();
        bp = pop16();
        (void)pop16();
        bx = pop16();
        dx = pop16();
        cx = pop16();
        ax = pop16();
      }
      break;
    }
    case 0x68:
      push_w(d.w32 ? imm32() : imm16());
      break;      // push imm
    case 0x69: {  // imul r, rm, imm
      modrm();
      if (d.w32) {
        int64_t prod =
            (int64_t)(int32_t)(d.is_mem ? rd32(d.mlin) : s->r[d.rm].e) * (int32_t)imm32();
        set_reg32((uint32_t)prod);
        flags_imul(prod, 4);
      } else {
        int32_t prod =
            (int32_t)(int16_t)(d.is_mem ? rd16(d.mlin) : s->r[d.rm].x) * (int16_t)imm16();
        set_reg16((uint16_t)prod);
        flags_imul(prod, 2);
      }
      break;
    }
    case 0x6a:
      push_w((uint32_t)(int32_t)(int8_t)imm8());
      break;      // push imm8 (sign-extended)
    case 0x6b: {  // imul r, rm, imm8
      modrm();
      if (d.w32) {
        int64_t prod = (int64_t)(int32_t)(d.is_mem ? rd32(d.mlin) : s->r[d.rm].e) * (int8_t)imm8();
        set_reg32((uint32_t)prod);
        flags_imul(prod, 4);
      } else {
        int32_t prod = (int32_t)(int16_t)(d.is_mem ? rd16(d.mlin) : s->r[d.rm].x) * (int8_t)imm8();
        set_reg16((uint16_t)prod);
        flags_imul(prod, 2);
      }
      break;
    }
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7a:
    case 0x7b:
    case 0x7c:
    case 0x7d:
    case 0x7e:
    case 0x7f: {  // jcc rel8
      int8_t rel = (int8_t)imm8();
      if (cond(op & 0xf)) d.nxt += (uint32_t)(int32_t)rel;
      break;
    }
    case 0x80:
      grp1(1, 2);
      break;
    case 0x81:
      grp1(d.w32 ? 4 : 2, 0);
      break;
    case 0x82:
      grp1(1, 2);
      break;
    case 0x83:
      grp1(d.w32 ? 4 : 2, 1);
      break;
    case 0x84: {
      modrm();
      uint8_t a = RM8(), b = reg8();
      flags_logic(a & b, 1);
      break;
    }  // test rm8, r8
    case 0x85: {
      modrm();
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        flags_logic(a & b, 4);
      } else {
        uint16_t a = RM16(), b = reg16();
        flags_logic(a & b, 2);
      }
      break;
    }  // test rm, r
    case 0x86: {
      modrm();
      uint8_t t = RM8();
      SET_RM8(reg8());
      set_reg8(t);
      break;
    }  // xchg rm8, r8
    case 0x87: {
      modrm();
      if (d.w32) {
        uint32_t t = RM32();
        SET_RM32(reg32());
        set_reg32(t);
      } else {
        uint16_t t = RM16();
        SET_RM16(reg16());
        set_reg16(t);
      }
      break;
    }  // xchg rm, r
    case 0x88: {
      modrm();
      SET_RM8(reg8());
      break;
    }  // mov rm8, r8
    case 0x89: {
      modrm();
      if (d.w32)
        SET_RM32(reg32());
      else
        SET_RM16(reg16());
      break;
    }  // mov rm, r
    case 0x8a: {
      modrm();
      set_reg8(RM8());
      break;
    }  // mov r8, rm8
    case 0x8b: {
      modrm();
      if (d.w32)
        set_reg32(RM32());
      else
        set_reg16(RM16());
      break;
    }  // mov r, rm
    case 0x8c: {
      modrm();
      if (d.w32)
        SET_RM32(s->sreg[d.reg]);
      else
        SET_RM16(s->sreg[d.reg]);
      break;
    }             // mov rm16, sreg
    case 0x8d: {  // lea r, m
      modrm();
      if (!d.is_mem) ud();
      if (d.w32)
        set_reg32(d.moff);
      else
        set_reg16((uint16_t)d.moff);
      break;
    }
    case 0x8e: {
      modrm();
      if (d.reg > gs_i) ud();
      load_seg(d.reg, rm16());
      break;
    }  // mov sreg, rm16
    case 0x8f: {
      modrm();
      uint32_t v = pop_w();
      if (d.w32)
        SET_RM32(v);
      else
        SET_RM16((uint16_t)v);
      break;
    }  // pop rm
    case 0x90:
      break;  // nop
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97: {  // xchg eAX, r
      if (d.w32) {
        uint32_t t = eax;
        eax = s->r[op & 7].e;
        s->r[op & 7].e = t;
      } else {
        uint16_t t = ax;
        ax = s->r[op & 7].x;
        s->r[op & 7].x = t;
      }
      break;
    }
    case 0x98:  // cbw/cwde: sign-extend AL->AX or AX->EAX
      if (d.w32)
        eax = (uint32_t)(int32_t)(int16_t)ax;
      else
        ax = (uint16_t)(int16_t)(int8_t)al;
      break;
    case 0x99:  // cwd/cdq: sign-extend AX->DX:AX or EAX->EDX:EAX
      if (d.w32)
        edx = (int32_t)eax < 0 ? 0xffffffffu : 0;
      else
        dx = (int16_t)ax < 0 ? 0xffffu : 0;
      break;
    case 0x9a: {  // call ptr16:16/32 — push CS, push return IP, load CS, jump
      uint32_t off = d.w32 ? imm32() : imm16();
      uint16_t sel = imm16();
      push_w(s->sreg[cs_i]);
      push_w((uint32_t)(fr->rec.pc + d.nxt));  // return: next instruction
      load_seg(cs_i, sel);
      eip = off;
      break;
    }
    case 0x9b:
      break;  // wait: no x87 in this machine
    case 0x9c:
      push_w(fl->word | 2);
      break;      // pushf
    case 0x9d: {  // popf: bit 1 stays set; RF/VM never load (SDM)
      uint32_t v = pop_w();
      if (d.w32)
        fl->word = (v & ~(0x30000u)) | 2;
      else
        fl->word = (fl->word & 0xffff0000u) | (v | 2);
      break;
    }
    case 0x9e: {  // sahf: AH -> SF ZF AF PF CF (SDM)
      uint32_t ahv = ah;
      fl->word = (fl->word & ~0xd5u) | (ahv & 0xd5u) | 2;
      break;
    }
    case 0x9f:
      ah = (uint8_t)(fl->word & 0xd5u) | 2;
      break;  // lahf
    case 0xa0: {
      uint64_t lin = s->base[d.seg >= 0 ? d.seg : ds_i] + (d.a32 ? imm32() : imm16());
      al = rd8(lin);
      break;
    }  // mov al, moffs8
    case 0xa1: {
      uint64_t lin = s->base[d.seg >= 0 ? d.seg : ds_i] + (d.a32 ? imm32() : imm16());
      if (d.w32)
        eax = rd32(lin);
      else
        ax = rd16(lin);
      break;
    }  // mov eAX, moffs
    case 0xa2: {
      uint64_t lin = s->base[d.seg >= 0 ? d.seg : ds_i] + (d.a32 ? imm32() : imm16());
      wr8(lin, al);
      break;
    }  // mov moffs8, al
    case 0xa3: {
      uint64_t lin = s->base[d.seg >= 0 ? d.seg : ds_i] + (d.a32 ? imm32() : imm16());
      if (d.w32)
        wr32(lin, eax);
      else
        wr16(lin, ax);
      break;
    }  // mov moffs, eAX
    case 0xa4:
    case 0xa5:
    case 0xa6:
    case 0xa7:
    case 0xaa:
    case 0xab:
    case 0xac:
    case 0xad:
    case 0xae:
    case 0xaf:
      string_op(op);
      break;
    case 0xa8: {
      uint8_t a = al, b = imm8();
      flags_logic(a & b, 1);
      break;
    }  // test al, imm8
    case 0xa9: {
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        flags_logic(a & b, 4);
      } else {
        uint16_t a = ax, b = imm16();
        flags_logic(a & b, 2);
      }
      break;
    }  // test eAX, imm
    case 0xb0:
    case 0xb1:
    case 0xb2:
    case 0xb3:
    case 0xb4:
    case 0xb5:
    case 0xb6:
    case 0xb7:  // mov r8, imm8
      if (op < 0xb4)
        s->r[op & 7].l = imm8();
      else
        s->r[op & 3].h = imm8();
      break;
    case 0xb8:
    case 0xb9:
    case 0xba:
    case 0xbb:
    case 0xbc:
    case 0xbd:
    case 0xbe:
    case 0xbf:  // mov r, imm
      if (d.w32)
        s->r[op & 7].e = imm32();
      else
        s->r[op & 7].x = imm16();
      break;
    case 0xc0:
      grp2(1, imm8());
      break;
    case 0xc1:
      grp2(d.w32 ? 4 : 2, imm8());
      break;
    case 0xc2: {  // ret imm16
      uint32_t n = imm16();
      eip = pop_w();
      if (d.w32)
        esp += n;
      else
        sp = (uint16_t)(sp + n);
      break;
    }
    case 0xc3:
      eip = pop_w();
      break;  // ret
    case 0xc4:
    case 0xc5: {  // les/lds r, m16:16/32
      modrm();
      if (!d.is_mem) ud();
      int seg = op == 0xc4 ? es_i : ds_i;
      if (d.w32) {
        set_reg32(RM32());
        load_seg(seg, rd16(d.mlin + 4));
      } else {
        set_reg16(RM16());
        load_seg(seg, rd16(d.mlin + 2));
      }
      break;
    }
    case 0xc6: {
      modrm();
      SET_RM8(imm8());
      break;
    }  // mov rm8, imm8
    case 0xc7: {
      modrm();
      if (d.w32)
        SET_RM32(imm32());
      else
        SET_RM16(imm16());
      break;
    }             // mov rm, imm
    case 0xc8: {  // enter imm16, imm8 (SDM ENTER)
      uint32_t alloc = imm16();
      uint32_t level = imm8() & 31;
      push_w(d.w32 ? ebp : bp);
      uint32_t frame = d.w32 ? esp : sp;
      if (level > 0) {
        for (uint32_t i = 1; i < level; i++) {
          if (d.w32) {
            ebp -= 4;
            esp = ebp;
            push32(rd32(s->base[ss_i] + esp + 4 - 4));
          } else {
            bp -= 2;
            sp = bp;
            push16(rd16(s->base[ss_i] + sp + 2 - 2));
          }
        }
        push_w(frame);
      }
      if (d.w32) {
        ebp = frame;
        esp -= alloc;
      } else {
        bp = (uint16_t)frame;
        sp = (uint16_t)(sp - alloc);
      }
      break;
    }
    case 0xc9: {  // leave: sp = bp, then pop bp
      if (d.w32) {
        esp = ebp;
        ebp = pop32();
      } else {
        sp = bp;
        bp = pop16();
      }
      break;
    }
    case 0xca: {  // retf imm16
      uint32_t n = imm16();
      eip = pop_w();
      load_seg(cs_i, (uint16_t)pop_w());
      if (d.w32)
        esp += n;
      else
        sp = (uint16_t)(sp + n);
      break;
    }
    case 0xcb:
      eip = pop_w();
      load_seg(cs_i, (uint16_t)pop_w());
      break;  // retf
    case 0xcc:
      do_int(3, (uint32_t)(fr->rec.pc + d.nxt));
      return;  // int3
    case 0xcd:
      do_int(imm8(), (uint32_t)(fr->rec.pc + d.nxt));
      return;  // int imm8
    case 0xce:
      if (fl->of) {
        do_int(4, (uint32_t)(fr->rec.pc + d.nxt));
        return;
      }
      break;      // into
    case 0xcf: {  // iret: pop (E)IP, CS, (E)FLAGS. RF/VM never load from the
                  // stored image (SDM IRET Operation: RF=0; VM stays 0 in
                  // real mode). 16-bit form loads only the low half.
      eip = pop_w();
      load_seg(cs_i, (uint16_t)pop_w());
      uint32_t flv = pop_w();
      if (d.w32)
        fl->word = (flv & ~(0x30000u)) | 2;
      else
        fl->word = (fl->word & 0xffff0000u) | ((flv & 0xffffu) | 2);
      break;
    }
    case 0xd0:
      grp2(1, 1);
      break;
    case 0xd1:
      grp2(d.w32 ? 4 : 2, 1);
      break;
    case 0xd2:
      grp2(1, cl);
      break;
    case 0xd3:
      grp2(d.w32 ? 4 : 2, cl);
      break;
    case 0xd4: {  // aam imm8: AH = AL/base, AL = AL%base (base 0 -> #DE)
      uint32_t base = imm8();
      if (base == 0) de();
      uint8_t q = al / (uint8_t)base;
      al = al % (uint8_t)base;
      ah = q;
      flags_logic(al, 1);
      break;
    }
    case 0xd5: {  // aad imm8: AL = (AL + AH*base) & 0xff, AH = 0
      uint32_t base = imm8();
      uint8_t r = (uint8_t)(al + ah * (uint8_t)base);
      al = r;
      ah = 0;
      flags_logic(r, 1);
      break;
    }
    case 0xd6:
      al = fl->cf ? 0xff : 0x00;
      break;      // salc (undocumented)
    case 0xd7: {  // xlat: AL = [seg:BX + AL]
      uint64_t lin = s->base[d.seg >= 0 ? d.seg : ds_i] + (d.a32 ? ebx : (uint32_t)bx) + al;
      al = rd8(lin);
      break;
    }
    case 0xd8:
    case 0xd9:
    case 0xda:
    case 0xdb:
    case 0xdc:
    case 0xdd:
    case 0xde:
    case 0xdf: {  // x87 escape
      modrm();
      // Only FNINIT (DB /3) is accepted; this machine has no FPU (D13).
      if (!(op == 0xdb && d.mod == 3 && d.reg == 3)) ud();
      break;
    }
    case 0xe0:
    case 0xe1:
    case 0xe2: {  // loopne/loope/loop rel8
      int8_t rel = (int8_t)imm8();
      uint32_t cnt = (d.w32 ? ecx : cx) - 1;
      if (d.w32)
        ecx = cnt;
      else
        cx = (uint16_t)cnt;
      int taken = cnt != 0 && (op == 0xe2 || cond(op == 0xe0 ? 5 : 4));
      if (taken) d.nxt += (uint32_t)(int32_t)rel;
      break;
    }
    case 0xe3: {  // jcxz/jecxz rel8 (counts at the ADDRESS size)
      int8_t rel = (int8_t)imm8();
      if ((d.a32 ? ecx : cx) == 0) d.nxt += (uint32_t)(int32_t)rel;
      break;
    }
    case 0xe4: {
      uint16_t port = imm8();
      al = (uint8_t)BusRead(cpu->io, port, 1);
      break;
    }  // in al, imm8
    case 0xe5: {
      uint16_t port = imm8();
      if (d.w32)
        eax = BusRead(cpu->io, port, 4);
      else
        ax = (uint16_t)BusRead(cpu->io, port, 2);
      break;
    }  // in eAX, imm8
    case 0xe6:
      BusWrite(cpu->io, imm8(), 1, al);
      break;  // out imm8, al
    case 0xe7:
      BusWrite(cpu->io, imm8(), d.w32 ? 4 : 2, d.w32 ? eax : ax);
      break;      // out imm8, eAX
    case 0xe8: {  // call rel16/32
      int32_t rel = d.w32 ? (int32_t)imm32() : (int16_t)imm16();
      push_w((uint32_t)(fr->rec.pc + d.nxt));  // return: next instruction
      d.nxt += (uint32_t)rel;
      break;
    }
    case 0xe9: {  // jmp rel16/32
      int32_t rel = d.w32 ? (int32_t)imm32() : (int16_t)imm16();
      d.nxt += (uint32_t)rel;
      break;
    }
    case 0xea: {  // jmp ptr16:16/32
      uint32_t off = d.w32 ? imm32() : imm16();
      load_seg(cs_i, imm16());
      eip = off;
      break;
    }
    case 0xeb: {
      int8_t rel = (int8_t)imm8();
      d.nxt += (uint32_t)(int32_t)rel;
      break;
    }  // jmp rel8
    case 0xec:
      al = (uint8_t)BusRead(cpu->io, dx, 1);
      break;  // in al, dx
    case 0xed:
      if (d.w32)
        eax = BusRead(cpu->io, dx, 4);
      else
        ax = (uint16_t)BusRead(cpu->io, dx, 2);
      break;  // in eAX, dx
    case 0xee:
      BusWrite(cpu->io, dx, 1, al);
      break;  // out dx, al
    case 0xef:
      BusWrite(cpu->io, dx, d.w32 ? 4 : 2, d.w32 ? eax : ax);
      break;      // out dx, eAX
    case 0xf4: {  // hlt: sleeps until an unmasked external interrupt (SDM)
      if (fl->if_ && cpu->int_ack) {
        cpu->wait = 1;
      } else {
        LogInfo("hlt with no wake sources, stopping emulation");
        cpu->halted = kCpuExited;
        cpu->exit_code = 0;
      }
      break;
    }
    case 0xf5:
      fl->cf = !fl->cf;
      break;  // cmc
    case 0xf8:
      fl->cf = 0;
      break;  // clc
    case 0xf9:
      fl->cf = 1;
      break;  // stc
    case 0xfa:
      fl->if_ = 0;
      break;    // cli
    case 0xfb:  // sti: the next instruction is not interruptible (SDM)
      fl->if_ = 1;
      s->intr_inhibit = 1;
      break;
    case 0xfc:
      fl->df = 0;
      break;  // cld
    case 0xfd:
      fl->df = 1;
      break;  // std
    case 0xf6:
      grp3(1);
      break;
    case 0xf7:
      grp3(d.w32 ? 4 : 2);
      break;
    case 0xfe: {  // grp4: inc/dec rm8 (reg field)
      modrm();
      if (d.reg > 1) ud();
      uint8_t v = RM8();
      uint8_t r = d.reg ? (uint8_t)(v - 1) : (uint8_t)(v + 1);
      SET_RM8(r);
      if (d.reg)
        flags_dec(r, 1);
      else
        flags_inc(r, 1);
      break;
    }
    case 0xff: {  // grp5: inc/dec/call/jmp/push (reg field)
      modrm();
      switch (d.reg) {
        case 0:
        case 1: {
          if (d.w32) {
            uint32_t v = RM32();
            uint32_t r = d.reg ? v - 1 : v + 1;
            SET_RM32(r);
            if (d.reg)
              flags_dec(r, 4);
            else
              flags_inc(r, 4);
          } else {
            uint16_t v = RM16();
            uint16_t r = (uint16_t)(d.reg ? v - 1 : v + 1);
            SET_RM16(r);
            if (d.reg)
              flags_dec(r, 2);
            else
              flags_inc(r, 2);
          }
          break;
        }
        case 2: {  // call near rm
          uint32_t t = d.w32 ? RM32() : RM16();
          push_w((uint32_t)(fr->rec.pc + d.nxt));
          eip = t;
          break;
        }
        case 3: {  // call far m16:16/32
          if (!d.is_mem) ud();
          uint32_t t = d.w32 ? rd32(d.mlin) : rd16(d.mlin);
          uint16_t sel = rd16(d.mlin + (d.w32 ? 4 : 2));
          push_w(s->sreg[cs_i]);
          push_w((uint32_t)(fr->rec.pc + d.nxt));  // return: next instruction,
                                                   // same as 0x9a/0xe8 (SDM pushes EIP past the call)
          load_seg(cs_i, sel);
          eip = t;
          break;
        }
        case 4:
          eip = d.w32 ? RM32() : RM16();
          break;   // jmp near rm
        case 5: {  // jmp far m16:16/32
          if (!d.is_mem) ud();
          uint32_t t = d.w32 ? rd32(d.mlin) : rd16(d.mlin);
          uint16_t sel = rd16(d.mlin + (d.w32 ? 4 : 2));
          load_seg(cs_i, sel);
          eip = t;
          break;
        }
        case 6:
          push_w(d.w32 ? RM32() : RM16());
          break;  // push rm
        default:
          ud();
      }
      break;
    }
    default:
      ud();
  }
}

// ---- machine irq sink ----------------------------------------------------------

void x86_set_intr(CpuState* c, int level) {
  x86_state* st = (x86_state*)c->priv;
  if (level) {
    st->intr_pending = 1;
    c->wait = 0;
  }
}

// ---- reset / boot ----------------------------------------------------------------

// Multiboot header magic the loader scans for (Multiboot 0.6.96 §3.1.1);
// the loader magic the kernel expects in EAX (§3.2).
enum { k_mb_hdr_magic = 0x1badb002, k_mb_load_magic = 0x2badb002 };

// How boards drive our interrupt lines: x86 has one shared INTR line, so the
// line id is ignored (the PIC supplies the vector on the INTA acknowledge).
// Boards call this hook instead of naming a CPU-model function.
static void x86_set_irq_line(CpuState* cpu, uint64_t line, int level) {
  (void)line;
  x86_set_intr(cpu, level);
}

void x86_init(CpuState* c) {
  x86_state* st;
  if (!c->priv) {
    st = (x86_state*)calloc(1, sizeof(x86_state));
    c->priv = st;
  } else {
    st = (x86_state*)c->priv;
    memset(st, 0, sizeof(*st));
  }
  st->r = (cell*)c->gpr;
  st->fl.word = 0x202;  // IF set, reserved bit 1 on (SDM reset state)
  c->set_irq = x86_set_irq_line;

  // Multiboot images (header in the first 8 KiB, 4-aligned) enter in flat
  // protected mode, CS=0x08/data=0x10 with built-in descriptors (QEMU
  // contract); everything else is a BIOS boot sector.
  int multiboot = 0;
  for (uint64_t a = c->image_base; a < c->image_base + 0x2000; a += 4)
    if (BusRead(c->bus, a, 4) == k_mb_hdr_magic) {
      multiboot = 1;
      break;
    }
  if (multiboot) {
    st->cr0 = 1;  // PE
    st->sreg[cs_i] = 0x08;
    st->base[cs_i] = 0;
    st->dbit[cs_i] = 1;
    for (int i = 0; i < 6; i++) {
      if (i == cs_i) continue;
      st->sreg[i] = 0x10;
      st->base[i] = 0;
      st->dbit[i] = 1;
    }
    st->r[eax_i].e = k_mb_load_magic;
    st->r[ebx_i].e = 0x7000;  // multiboot info scratch
  } else {
    // BIOS boot-sector handoff: DL = 0x80 (boot drive), IVT at linear 0.
    st->r[edx_i].e = 0x80;
    st->idtr = 0;
    st->idtr_limit = 0x3ff;
  }
}

void x86_dump_regs(const CpuState* c) {
  const x86_state* st = (const x86_state*)c->priv;
  char buf[160];
  static const char names[8][4] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
  for (int i = 0; i < 8; i += 4) {
    int n = snprintf(buf, sizeof(buf), "%s=%08x %s=%08x %s=%08x %s=%08x\n", names[i], st->r[i].e,
                     names[i + 1], st->r[i + 1].e, names[i + 2], st->r[i + 2].e, names[i + 3],
                     st->r[i + 3].e);
    HostWriteErr(buf, (size_t)n);
  }
  static const char snames[6][3] = {"es", "cs", "ss", "ds", "fs", "gs"};
  int n = snprintf(buf, sizeof(buf),
                   "eip=%08x eflags=%08x cr0=%08x %s=%04x %s=%04x %s=%04x "
                   "%s=%04x %s=%04x %s=%04x\n",
                   (unsigned)c->pc, st->fl.word, st->cr0, snames[0], st->sreg[0], snames[1],
                   st->sreg[1], snames[2], st->sreg[2], snames[3], st->sreg[3], snames[4],
                   st->sreg[4], snames[5], st->sreg[5]);
  HostWriteErr(buf, (size_t)n);
}
