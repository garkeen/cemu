// The x87 FPU and its SSE-visible state (SDM vol.1 ch.8,10,11). The
// arithmetic itself is Berkeley SoftFloat (softfloat/softfloat.c), the same
// package QEMU's target/i386/tcg/fpu_helper.c uses; the register stack, tags,
// control/status words, exception model and the FSTENV/FNSAVE/FXSAVE images
// are the architecture's, written here.
//
// Transcendental instructions (F2XM1, FYL2X, FPATAN, FSIN, FCOS, FSINCOS,
// FPTAN, FYL2XP1) compute through the host's long double, which on this
// toolchain is the x87 80-bit type. QEMU's helpers for the same instructions
// convert to double and call the host libm (fpu_helper.c helper_fsin/cos/tan),
// so this is the same class of approximation with more mantissa bits; the
// exact bit-for-bit Intel algorithms are not reproduced.
#include <math.h>
#include <string.h>

#include "cpu/isa/x86/fpu.h"

static const extFloat80_t kIndefinite = {0xc000000000000000ULL, 0xffff};
static const extFloat80_t kOne = {0x8000000000000000ULL, 0x3fff};
static const extFloat80_t kZero = {0, 0};
// The FLD1..FLDZ constants (D9 E8..EE), correctly rounded to the 64-bit
// significand (SDM vol.1 8.1.2 "Loading Constants"; the values are the
// manual's and match QEMU target/i386/tcg/fpu_helper.c).
static const extFloat80_t kFldConst[7] = {
    {0x8000000000000000ULL, 0x3fff},  // 1.0
    {0xd49a784bcd1b8afeULL, 0x4000},  // log2(10)
    {0xb8aa3b295c17f0bcULL, 0x3fff},  // log2(e)
    {0xc90fdaa22168c235ULL, 0x4000},  // pi
    {0x9a209a84fbcff799ULL, 0x3ffd},  // log10(2)
    {0xb17217f7d1cf79acULL, 0x3ffe},  // ln(2)
    {0, 0},                           // +0
};

static extFloat80_t F80(uint64_t signif, uint16_t sign_exp) {
  extFloat80_t v;
  v.signif = signif;
  v.signExp = sign_exp;
  return v;
}

static int F80IsZero(extFloat80_t v) {
  return (v.signExp & 0x7fff) == 0 && v.signif == 0;
}
static int F80IsInf(extFloat80_t v) {
  return (v.signExp & 0x7fff) == 0x7fff && v.signif == 0x8000000000000000ULL;
}
static int F80IsNan(extFloat80_t v) {
  return (v.signExp & 0x7fff) == 0x7fff && v.signif != 0x8000000000000000ULL;
}
// A NaN whose integer bit is set but whose quiet bit is clear is signaling
// (SDM vol.1 4.2.2); every other NaN is quiet.
static int F80IsSnan(extFloat80_t v) {
  return F80IsNan(v) && !(v.signif & 0x4000000000000000ULL);
}
static int F80IsDenormal(extFloat80_t v) {
  return (v.signExp & 0x7fff) == 0 && v.signif != 0;
}
static int F80Sign(extFloat80_t v) { return (v.signExp >> 15) & 1; }

static void UpdateEs(x86_fpu* f);

int fpu_top(const x86_fpu* f) { return (int)((f->sw >> kFpuTopShift) & 7); }

void fpu_set_top(x86_fpu* f, int top) {
  f->sw = (uint16_t)((f->sw & ~kFpuTopMask) | ((top & 7) << kFpuTopShift));
}

int fpu_tag(const x86_fpu* f, int physical) {
  return (f->tw >> (2 * physical)) & 3;
}

static int TagOf(extFloat80_t v) {
  if (F80IsZero(v)) return kFpuTagZero;
  if (F80IsNan(v) || F80IsInf(v) || F80IsDenormal(v)) return kFpuTagSpecial;
  return kFpuTagValid;
}

void fpu_mark(x86_fpu* f, int physical) {
  int t = TagOf(f->st[physical]);
  f->tw = (uint16_t)((f->tw & ~(3u << (2 * physical))) | ((unsigned)t << (2 * physical)));
}

void fpu_set_tags(x86_fpu* f, uint16_t tw) { f->tw = tw; }

void fpu_refresh(x86_fpu* f) { UpdateEs(f); }

// FLDCW/FLDENV/FXRSTOR load the control word with bit 6 forced to 1 and the
// reserved bits 15:13 cleared. SDM vol.1 8.1.5 gives bit 6 as reserved (1 on
// 287 and later); Bochs control_w.h FPU_CW_Reserved_Bits = 0xe0c0 and
// fpu.cc applies exactly this mask on FLDCW and environment loads.
void fpu_load_cw(x86_fpu* f, uint16_t cw) {
  f->cw = (uint16_t)((cw & ~0xe0c0u) | 0x0040u);
  UpdateEs(f);
}

// FNSTENV/FSTENV mask all exceptions after the image has been stored
// (SDM vol.2 FSTENV/FNSTENV: "Then mask all floating-point exceptions";
// Bochs fpu.cc FNSTENV does `FPU_CONTROL_WORD |= FPU_CW_Exceptions_Mask`).
// The exception flags stay in SW; only their unmasked state (and therefore
// ES/#MF) goes away.
void fpu_mask_exceptions(x86_fpu* f) {
  f->cw |= 0x003fu;
  UpdateEs(f);
}

static void UpdateEs(x86_fpu* f) {
  uint16_t raised = f->sw & (kFpuIe | kFpuDe | kFpuZe | kFpuOe | kFpuUe | kFpuPe);
  uint16_t unmasked = raised & (uint16_t)~f->cw & (kFpuIe | kFpuDe | kFpuZe | kFpuOe | kFpuUe | kFpuPe);
  // B (bit 15) is the 8087-compatibility busy bit and always mirrors ES
  // (SDM vol.1 8.1.3, Figure 8-4; Intel387 datasheet 3.2.1). ES itself is
  // not a storage bit: it follows the flags and masks, including after
  // FLDENV/FRSTOR replace the status word.
  f->sw = (uint16_t)((f->sw & ~(kFpuEs | kFpuB)) | (unmasked ? kFpuEs | kFpuB : 0));
  f->pending_mf = unmasked != 0;
}

void fpu_begin(x86_fpu* f) {
  // RC (CW[11:10]): 00 nearest, 01 down (toward -inf), 10 up (toward +inf),
  // 11 truncate (SDM vol.1 8.1.5); v86 fpu.rs set_control_word maps the same.
  static const uint_fast8_t kRc[4] = {softfloat_round_near_even, softfloat_round_min,
                                     softfloat_round_max, softfloat_round_minMag};
  // Precision control: 00 single, 01 reserved (extended, QEMU), 10 double,
  // 11 extended (SDM vol.1 8.1.5; QEMU fpu_helper.c set_floatx80_rounding_precision).
  static const uint_fast8_t kPc[4] = {32, 80, 64, 80};
  softfloat_roundingMode = kRc[(f->cw >> 10) & 3];
  extF80_roundingPrecision = kPc[(f->cw >> 8) & 3];
  softfloat_exceptionFlags = 0;
}

void fpu_end(x86_fpu* f) {
  uint_fast8_t sf = softfloat_exceptionFlags;
  uint16_t st = 0;
  if (sf & softfloat_flag_invalid) st |= kFpuIe;
  if (sf & softfloat_flag_infinite) st |= kFpuZe;
  if (sf & softfloat_flag_overflow) st |= kFpuOe;
  if (sf & softfloat_flag_underflow) st |= kFpuUe;
  if (sf & softfloat_flag_inexact) st |= kFpuPe;
  softfloat_exceptionFlags = 0;
  f->sw |= st;
  UpdateEs(f);
}

void fpu_init(x86_fpu* f) {
  memset(f, 0, sizeof(*f));
  f->cw = 0x037f;  // all exceptions masked, extended precision, nearest
  f->tw = 0xffff;  // every register empty
  f->mxcsr = 0x1f80;
  f->mxcsr_mask = 0xffff;
}

void fpu_finit(x86_fpu* f) {
  f->cw = 0x037f;
  f->sw = 0;  // TOP = 0 after FNINIT (SDM FNINIT)
  f->tw = 0xffff;
  f->fip = f->fdp = 0;
  f->fcs = f->fds = 0;
  f->fop = 0;
  f->pending_mf = 0;
}

extFloat80_t fpu_get(x86_fpu* f, int i) {
  int p = (fpu_top(f) + i) & 7;
  if (fpu_tag(f, p) == kFpuTagEmpty) {
    // Stack underflow is an invalid operation (SDM vol.1 8.7.2): IE and SF
    // set, C1 cleared; the masked result is the indefinite QNaN.
    f->sw |= kFpuIe | kFpuSf;
    f->sw &= (uint16_t)~kFpuC1;
    UpdateEs(f);
    return kIndefinite;
  }
  return f->st[p];
}

void fpu_set(x86_fpu* f, int i, extFloat80_t v) {
  int p = (fpu_top(f) + i) & 7;
  f->st[p] = v;
  fpu_mark(f, p);
}

void fpu_push(x86_fpu* f, extFloat80_t v) {
  int t = fpu_top(f);
  int nt = (t - 1) & 7;
  if (fpu_tag(f, nt) != kFpuTagEmpty) {
    // Stack overflow: IE|SF, C1=1 (SDM vol.1 8.7.2); masked, the push stores
    // the indefinite QNaN like the hardware does.
    f->sw |= kFpuIe | kFpuSf | kFpuC1;
    v = kIndefinite;
    UpdateEs(f);
  }
  f->st[nt] = v;
  fpu_mark(f, nt);
  fpu_set_top(f, nt);
}

void fpu_pop(x86_fpu* f) {
  int t = fpu_top(f);
  if (fpu_tag(f, t) == kFpuTagEmpty) {
    f->sw |= kFpuIe | kFpuSf;
    f->sw &= (uint16_t)~kFpuC1;
    UpdateEs(f);
  }
  f->tw = (uint16_t)((f->tw & ~(3u << (2 * t))) | (kFpuTagEmpty << (2 * t)));
  fpu_set_top(f, (t + 1) & 7);
}

void fpu_free(x86_fpu* f, int i) {
  int p = (fpu_top(f) + i) & 7;
  f->tw = (uint16_t)((f->tw & ~(3u << (2 * p))) | (kFpuTagEmpty << (2 * p)));
}

void fpu_exchange(x86_fpu* f, int i) {
  int t = fpu_top(f);
  int p = (t + i) & 7;
  int et = fpu_tag(f, t) == kFpuTagEmpty;
  int ep = fpu_tag(f, p) == kFpuTagEmpty;
  extFloat80_t a = f->st[t], b = f->st[p];
  if (et || ep) {
    // SDM FXCH "Numeric Exceptions": #IS when either register is empty.
    // Bochs fpu_misc.cc FXCH_STi sets IE|SF and, if masked, substitutes the
    // indefinite QNaN for the empty side before exchanging.
    f->sw |= kFpuIe | kFpuSf;
    if (et) a = kIndefinite;
    if (ep) b = kIndefinite;
    UpdateEs(f);
  }
  f->st[t] = b;
  f->st[p] = a;
  fpu_mark(f, t);
  fpu_mark(f, p);
  f->sw &= (uint16_t)~kFpuC1;
}

static void DenormalOperand(x86_fpu* f, extFloat80_t a) {
  if (F80IsDenormal(a)) {
    f->sw |= kFpuDe;
    UpdateEs(f);
  }
}
static void DenormalOperands(x86_fpu* f, extFloat80_t a, extFloat80_t b) {
  DenormalOperand(f, a);
  DenormalOperand(f, b);
}

void fpu_arith(x86_fpu* f, int op, int dst, extFloat80_t b) {
  extFloat80_t a = fpu_get(f, 0);
  extFloat80_t r = kIndefinite;
  DenormalOperands(f, a, b);
  fpu_begin(f);
  switch (op) {
    case kFAdd: extF80M_add(&a, &b, &r); break;
    case kFMul: extF80M_mul(&a, &b, &r); break;
    case kFSub: extF80M_sub(&a, &b, &r); break;
    case kFSubR: extF80M_sub(&b, &a, &r); break;
    case kFDiv: extF80M_div(&a, &b, &r); break;
    case kFDivR: extF80M_div(&b, &a, &r); break;
    default: r = kIndefinite; break;  // callers route the compare forms elsewhere
  }
  fpu_end(f);
  fpu_set(f, dst, r);
}

// The status/C-bit image of FCOM/FUCOM (SDM FCOM): C3 equal, C0 less,
// C3|C2|C0 unordered. Returns the four-bit mask, C1 excluded.
static uint32_t CompareBits(extFloat80_t a, extFloat80_t b) {
  if (F80IsNan(a) || F80IsNan(b)) return kFpuC3 | kFpuC2 | kFpuC0;
  int az = F80IsZero(a), bz = F80IsZero(b);
  if (az && bz) return kFpuC3;
  int sa = F80Sign(a), sb = F80Sign(b);
  if (sa != sb) return sa ? kFpuC0 : 0;
  uint32_t ae = a.signExp & 0x7fff, be = b.signExp & 0x7fff;
  int less;
  if (ae != be) {
    less = ae < be;
  } else if (a.signif != b.signif) {
    less = a.signif < b.signif;
  } else {
    return kFpuC3;
  }
  if (sa) less = !less;
  return less ? kFpuC0 : 0;
}

void fpu_compare(x86_fpu* f, extFloat80_t b, int quiet) {
  extFloat80_t a = fpu_get(f, 0);
  DenormalOperands(f, a, b);  // FCOM/FUCOM/FICOM: #D on a denormal operand
  int invalid = F80IsNan(a) || F80IsNan(b) ? (!quiet || F80IsSnan(a) || F80IsSnan(b)) : 0;
  if (invalid) f->sw |= kFpuIe;
  f->sw &= (uint16_t)~(kFpuC0 | kFpuC1 | kFpuC2 | kFpuC3);
  if (F80IsNan(a) || F80IsNan(b))
    f->sw |= kFpuC3 | kFpuC2 | kFpuC0;
  else
    f->sw |= (uint16_t)CompareBits(a, b);
  UpdateEs(f);
}

uint32_t fpu_compare_i(x86_fpu* f, extFloat80_t b, int quiet) {
  enum { kCf = 1u << 0, kPf = 1u << 2, kZf = 1u << 6 };
  extFloat80_t a = fpu_get(f, 0);
  DenormalOperands(f, a, b);  // FCOMI/FUCOMI: #D on a denormal operand
  int unordered = F80IsNan(a) || F80IsNan(b);
  int invalid = unordered ? (!quiet || F80IsSnan(a) || F80IsSnan(b)) : 0;
  if (invalid) f->sw |= kFpuIe;
  UpdateEs(f);
  if (unordered) return kCf | kPf | kZf;
  uint32_t cc = CompareBits(a, b);
  if (cc & kFpuC3) return kZf;
  if (cc & kFpuC0) return kCf;
  return 0;
}

void fpu_test(x86_fpu* f) { fpu_compare(f, kZero, 0); }

void fpu_examine(x86_fpu* f) {
  // FXAM inspects an empty register without raising the stack fault FCOM
  // would (QEMU helper_fxam_ST0 reads the raw slot and tests the tag).
  int p = fpu_top(f);
  extFloat80_t v = f->st[p];
  f->sw &= (uint16_t)~(kFpuC0 | kFpuC1 | kFpuC2 | kFpuC3);
  if (F80Sign(v)) f->sw |= kFpuC1;  // QEMU: FXAM puts the sign in C1
  if (fpu_tag(f, p) == kFpuTagEmpty) {
    // SDM vol.2 FXAM table 3-42: Empty is C3,C2,C0 = 1,0,1. Setting C2 as well
    // reported 1,1,1 (an encoding no class uses) and made an empty ST(0)
    // indistinguishable from nothing in particular.
    f->sw |= kFpuC3 | kFpuC0;
  } else if (F80IsNan(v) && (v.signif & 0x8000000000000000ULL)) {
    f->sw |= kFpuC0;
  } else if (F80IsNan(v)) {
    // A mantissa with the integer bit clear is the "unsupported" class
    // (SDM vol.2 FXAM table 3-42: C3,C2,C0 = 0,0,0), not a NaN. QEMU's
    // helper_fxam_ST0 leaves all four C bits clear for exactly this case;
    // treating it as a NaN reported 0,0,1.
  } else if (F80IsZero(v)) {
    f->sw |= kFpuC3;
  } else if (F80IsInf(v)) {
    f->sw |= kFpuC2 | kFpuC0;
  } else if (F80IsDenormal(v)) {
    f->sw |= kFpuC3 | kFpuC2;
  } else {
    f->sw |= kFpuC2;
  }
}

void fpu_change_sign(x86_fpu* f) {
  extFloat80_t v = fpu_get(f, 0);
  v.signExp ^= 0x8000;
  fpu_set(f, 0, v);
  f->sw &= (uint16_t)~kFpuC1;
}

void fpu_abs(x86_fpu* f) {
  extFloat80_t v = fpu_get(f, 0);
  v.signExp &= 0x7fff;
  fpu_set(f, 0, v);
  f->sw &= (uint16_t)~kFpuC1;
}

void fpu_load_const(x86_fpu* f, int which) {
  fpu_push(f, kFldConst[which]);
}

// ---- host long double <-> extFloat80 ----------------------------------------

static long double F80ToLd(extFloat80_t v) {
  union {
    long double ld;
    uint8_t b[sizeof(long double)];
  } u;
  memset(&u, 0, sizeof(u));
  memcpy(u.b, &v, 10);
  return u.ld;
}

static extFloat80_t LdToF80(long double x) {
  union {
    long double ld;
    uint8_t b[sizeof(long double)];
  } u;
  extFloat80_t v;
  u.ld = x;
  memcpy(&v, u.b, 10);
  return v;
}

static long double Ln2(void) { return 0.693147180559945309417232121458176568L; }

void fpu_f2xm1(x86_fpu* f) {
  extFloat80_t a = fpu_get(f, 0);
  DenormalOperand(f, a);  // F2XM1: #D on a denormal source
  if (F80IsNan(a)) {
    if (F80IsSnan(a)) {
      f->sw |= kFpuIe;
      a.signif |= 0x4000000000000000ULL;
    }
    fpu_set(f, 0, a);
    UpdateEs(f);
    return;
  }
  long double x = F80ToLd(a);
  if (fabsl(x) > 1.0L) {
    f->sw |= kFpuIe;
    fpu_set(f, 0, kIndefinite);
    UpdateEs(f);
    return;
  }
  fpu_begin(f);
  extFloat80_t r = LdToF80(expm1l(x * Ln2()));
  softfloat_exceptionFlags |= softfloat_flag_inexact;
  fpu_end(f);
  fpu_set(f, 0, r);
}

void fpu_fyl2x(x86_fpu* f) {
  extFloat80_t x = fpu_get(f, 0);
  extFloat80_t y = fpu_get(f, 1);
  DenormalOperands(f, x, y);  // FYL2X: #D on either denormal source
  if (F80IsNan(x) || F80IsNan(y)) {
    if (F80IsSnan(x) || F80IsSnan(y)) f->sw |= kFpuIe;
    fpu_set(f, 1, F80IsNan(x) ? x : y);
    fpu_pop(f);
    UpdateEs(f);
    return;
  }
  extFloat80_t r;
  if (F80IsZero(x)) {
    // log2(0) = -inf and raises ZE; y * (-inf) is then the sign rule of a
    // multiply (y = 0 turns it into the invalid indefinite NaN).
    f->sw |= kFpuZe;
    if (F80IsZero(y)) {
      f->sw |= kFpuIe;
      r = kIndefinite;
    } else {
      r = F80(0x8000000000000000ULL, (uint16_t)(0x7fff | (F80Sign(y) ? 0 : 0x8000)));
    }
    fpu_set(f, 1, r);
    fpu_pop(f);
    UpdateEs(f);
    return;
  }
  if (F80Sign(x)) {
    f->sw |= kFpuIe;
    fpu_set(f, 1, kIndefinite);
    fpu_pop(f);
    UpdateEs(f);
    return;
  }
  fpu_begin(f);
  r = LdToF80(F80ToLd(y) * log2l(F80ToLd(x)));
  softfloat_exceptionFlags |= softfloat_flag_inexact;
  fpu_end(f);
  fpu_set(f, 1, r);
  fpu_pop(f);
}

void fpu_fptan(x86_fpu* f) {
  extFloat80_t a = fpu_get(f, 0);
  DenormalOperand(f, a);  // FPTAN: #D on a denormal source
  if (F80IsNan(a)) {
    if (F80IsSnan(a)) f->sw |= kFpuIe;
    fpu_set(f, 0, a);
    UpdateEs(f);
    return;
  }
  long double x = F80ToLd(a);
  if (fabsl(x) >= 0x1p63L) {
    f->sw |= kFpuC2;
    UpdateEs(f);
    return;
  }
  fpu_begin(f);
  extFloat80_t r = LdToF80(tanl(x));
  softfloat_exceptionFlags |= softfloat_flag_inexact;
  fpu_end(f);
  fpu_set(f, 0, r);
  fpu_push(f, kOne);
  f->sw &= (uint16_t)~kFpuC2;
}

void fpu_fpatan(x86_fpu* f) {
  extFloat80_t x = fpu_get(f, 0);   // ST0
  extFloat80_t y = fpu_get(f, 1);   // ST1
  DenormalOperands(f, x, y);  // FPATAN: #D on either denormal source
  if (F80IsNan(x) || F80IsNan(y)) {
    if (F80IsSnan(x) || F80IsSnan(y)) f->sw |= kFpuIe;
    fpu_set(f, 1, F80IsNan(x) ? x : y);
    fpu_pop(f);
    UpdateEs(f);
    return;
  }
  fpu_begin(f);
  // atan2(ST1, ST0): the encoding's y is ST1, x is ST0 (SDM FPATAN).
  extFloat80_t r = LdToF80(atan2l(F80ToLd(y), F80ToLd(x)));
  softfloat_exceptionFlags |= softfloat_flag_inexact;
  fpu_end(f);
  fpu_set(f, 1, r);
  fpu_pop(f);
}

void fpu_fxtract(x86_fpu* f) {
  extFloat80_t v = fpu_get(f, 0);
  DenormalOperand(f, v);  // FXTRACT: #D on a denormal source
  if (F80IsZero(v)) {
    f->sw |= kFpuZe;
    fpu_set(f, 0, F80(0x8000000000000000ULL, 0xffff));  // -inf
    fpu_push(f, F80(0, 0));
    UpdateEs(f);
    return;
  }
  if (F80IsInf(v)) {
    fpu_set(f, 0, v);
    fpu_push(f, v);
    return;
  }
  if (F80IsNan(v)) {
    if (F80IsSnan(v)) {
      f->sw |= kFpuIe;
      v.signif |= 0x4000000000000000ULL;
    }
    fpu_set(f, 0, v);
    fpu_push(f, v);
    UpdateEs(f);
    return;
  }
  long double x = F80ToLd(v);
  int e;
  long double m = frexpl(x, &e);  // x = m * 2^e, |m| in [0.5,1)
  fpu_begin(f);
  extFloat80_t ex = LdToF80((long double)(e - 1));
  extFloat80_t sig = LdToF80(m * 2.0L);
  fpu_end(f);
  fpu_set(f, 0, sig);
  fpu_push(f, ex);
  // After the push: ST0 = exponent, ST1 = significand (SDM FXTRACT).
}

void fpu_fprem(x86_fpu* f, int ieee) {
  extFloat80_t a = fpu_get(f, 0);
  extFloat80_t b = fpu_get(f, 1);
  DenormalOperands(f, a, b);  // FPREM/FPREM1: #D on either denormal source
  f->sw &= (uint16_t)~(kFpuC0 | kFpuC1 | kFpuC2 | kFpuC3);
  if (F80IsNan(a) || F80IsNan(b) || F80IsZero(b) || F80IsInf(a) || F80IsZero(a)) {
    fpu_begin(f);
    extFloat80_t r;
    extF80M_rem(&a, &b, &r);
    fpu_end(f);
    fpu_set(f, 0, r);
    UpdateEs(f);
    return;
  }
  long double la = F80ToLd(a), lb = F80ToLd(b);
  int ea, eb;
  frexpl(la, &ea);
  frexpl(lb, &eb);
  int expdiff = ea - eb;
  if (expdiff < 64) {
    // Full quotient: both operands' significands fit; the host long double's
    // 64-bit mantissa holds the integer quotient exactly, so the low three
    // bits are the architecture's Q0-Q2.
    long double ratio = la / lb;
    // FPREM1 rounds the quotient to nearest, ties to even (IEEE remainder,
    // SDM FPREM1); FPREM truncates toward zero (SDM FPREM). roundl() would
    // round a .5 quotient away from zero and report the wrong Q0-Q2.
    long double q = ieee ? rintl(ratio) : truncl(ratio);
    fpu_begin(f);
    extFloat80_t r;
    extF80M_rem(&a, &b, &r);
    fpu_end(f);
    fpu_set(f, 0, r);
    uint64_t qi = (uint64_t)(int64_t)q;
    if (qi & 1) f->sw |= kFpuC1;
    if (qi & 2) f->sw |= kFpuC3;
    if (qi & 4) f->sw |= kFpuC0;
  } else {
    // Partial remainder (AMD manual's choice, followed by Intel and QEMU):
    // scale the divisor so the exponent difference is 32..63, take one
    // remainder and set C2 to ask the software to continue.
    int n = 32 + (expdiff % 32);
    int shift = expdiff - n;
    extFloat80_t scaled = b;
    int se = (scaled.signExp & 0x7fff);
    long double sl = ldexpl(F80ToLd(b), shift);
    scaled = LdToF80(sl);
    (void)se;
    fpu_begin(f);
    extFloat80_t r;
    extF80M_rem(&a, &scaled, &r);
    fpu_end(f);
    fpu_set(f, 0, r);
    f->sw |= kFpuC2;
  }
  UpdateEs(f);
}

void fpu_fdecstp(x86_fpu* f) {
  fpu_set_top(f, (fpu_top(f) - 1) & 7);
  f->sw &= (uint16_t)~kFpuC1;
}

void fpu_fincstp(x86_fpu* f) {
  fpu_set_top(f, (fpu_top(f) + 1) & 7);
  f->sw &= (uint16_t)~kFpuC1;
}

void fpu_fyl2xp1(x86_fpu* f) {
  extFloat80_t x = fpu_get(f, 0);
  extFloat80_t y = fpu_get(f, 1);
  DenormalOperands(f, x, y);  // FYL2XP1: #D on either denormal source
  if (F80IsNan(x) || F80IsNan(y)) {
    if (F80IsSnan(x) || F80IsSnan(y)) f->sw |= kFpuIe;
    fpu_set(f, 1, F80IsNan(x) ? x : y);
    fpu_pop(f);
    UpdateEs(f);
    return;
  }
  // ST0 must be in [-(1-sqrt(2)/2), 1-sqrt(2)/2] (AMD allows the wider
  // sqrt(2)-1 range, which QEMU also accepts).
  long double lx = F80ToLd(x);
  if (fabsl(lx) >= 0.2928932188134524755991556378951509607152L) {
    f->sw |= kFpuIe;
    fpu_set(f, 1, kIndefinite);
    fpu_pop(f);
    UpdateEs(f);
    return;
  }
  fpu_begin(f);
  extFloat80_t r = LdToF80(F80ToLd(y) * (log1pl(lx) / Ln2()));
  softfloat_exceptionFlags |= softfloat_flag_inexact;
  fpu_end(f);
  fpu_set(f, 1, r);
  fpu_pop(f);
}

void fpu_fsqrt(x86_fpu* f) {
  extFloat80_t a = fpu_get(f, 0);
  DenormalOperand(f, a);  // FSQRT: #D on a denormal source
  f->sw &= (uint16_t)~(kFpuC0 | kFpuC1 | kFpuC2 | kFpuC3);
  if (F80Sign(a) && !F80IsZero(a) && !F80IsNan(a)) f->sw |= kFpuC2;
  fpu_begin(f);
  extFloat80_t r;
  extF80M_sqrt(&a, &r);
  fpu_end(f);
  fpu_set(f, 0, r);
}

void fpu_fsincos(x86_fpu* f) {
  extFloat80_t a = fpu_get(f, 0);
  DenormalOperand(f, a);  // FSINCOS: #D on a denormal source
  if (F80IsNan(a)) {
    if (F80IsSnan(a)) f->sw |= kFpuIe;
    fpu_set(f, 0, a);
    UpdateEs(f);
    return;
  }
  long double x = F80ToLd(a);
  if (fabsl(x) >= 0x1p63L) {
    f->sw |= kFpuC2;
    UpdateEs(f);
    return;
  }
  fpu_begin(f);
  extFloat80_t s = LdToF80(sinl(x));
  extFloat80_t c = LdToF80(cosl(x));
  softfloat_exceptionFlags |= softfloat_flag_inexact;
  fpu_end(f);
  fpu_set(f, 0, s);
  fpu_push(f, c);
  f->sw &= (uint16_t)~kFpuC2;
}

void fpu_frndint(x86_fpu* f) {
  extFloat80_t a = fpu_get(f, 0);
  DenormalOperand(f, a);  // FRNDINT: #D on a denormal source
  fpu_begin(f);
  extFloat80_t r;
  extF80M_roundToInt(&a, softfloat_roundingMode, true, &r);
  fpu_end(f);
  fpu_set(f, 0, r);
}

void fpu_fscale(x86_fpu* f) {
  extFloat80_t a = fpu_get(f, 0);
  extFloat80_t b = fpu_get(f, 1);
  DenormalOperands(f, a, b);  // FSCALE: #D on either denormal source
  if (F80IsNan(a) || F80IsNan(b)) {
    if (F80IsSnan(a) || F80IsSnan(b)) f->sw |= kFpuIe;
    fpu_set(f, 0, F80IsNan(a) ? a : b);
    UpdateEs(f);
    return;
  }
  long double lb = F80ToLd(b);
  if (lb > 2147483647.0L) lb = 2147483647.0L;
  if (lb < -2147483648.0L) lb = -2147483648.0L;
  int n = (int)truncl(lb);
  if (n >= -16445 && n <= 16383) {
    extFloat80_t p = F80(0x8000000000000000ULL, (uint16_t)(0x3fff + n));
    fpu_begin(f);
    extFloat80_t r;
    extF80M_mul(&a, &p, &r);
    fpu_end(f);
    fpu_set(f, 0, r);
  } else if (n > 16383) {
    if (F80IsZero(a) || F80IsInf(a)) return;
    f->sw |= kFpuOe | kFpuPe;
    fpu_set(f, 0, F80(0x8000000000000000ULL, (uint16_t)(0x7fff | (a.signExp & 0x8000))));
    UpdateEs(f);
  } else {
    if (F80IsZero(a) || F80IsInf(a)) return;
    f->sw |= kFpuUe | kFpuPe;
    fpu_set(f, 0, F80(0, (uint16_t)(a.signExp & 0x8000)));
    UpdateEs(f);
  }
}

void fpu_fsin(x86_fpu* f) {
  extFloat80_t a = fpu_get(f, 0);
  DenormalOperand(f, a);  // FSIN: #D on a denormal source
  if (F80IsNan(a)) {
    if (F80IsSnan(a)) f->sw |= kFpuIe;
    fpu_set(f, 0, a);
    UpdateEs(f);
    return;
  }
  long double x = F80ToLd(a);
  if (fabsl(x) >= 0x1p63L) {
    f->sw |= kFpuC2;
    UpdateEs(f);
    return;
  }
  fpu_begin(f);
  extFloat80_t r = LdToF80(sinl(x));
  softfloat_exceptionFlags |= softfloat_flag_inexact;
  fpu_end(f);
  fpu_set(f, 0, r);
  f->sw &= (uint16_t)~kFpuC2;
}

void fpu_fcos(x86_fpu* f) {
  extFloat80_t a = fpu_get(f, 0);
  DenormalOperand(f, a);  // FCOS: #D on a denormal source
  if (F80IsNan(a)) {
    if (F80IsSnan(a)) f->sw |= kFpuIe;
    fpu_set(f, 0, a);
    UpdateEs(f);
    return;
  }
  long double x = F80ToLd(a);
  if (fabsl(x) >= 0x1p63L) {
    f->sw |= kFpuC2;
    UpdateEs(f);
    return;
  }
  fpu_begin(f);
  extFloat80_t r = LdToF80(cosl(x));
  softfloat_exceptionFlags |= softfloat_flag_inexact;
  fpu_end(f);
  fpu_set(f, 0, r);
  f->sw &= (uint16_t)~kFpuC2;
}

// ---- conversions ------------------------------------------------------------

extFloat80_t fpu_load_f32(x86_fpu* f, uint32_t bits) {
  // A load does not raise the denormal-operand exception; the arithmetic
  // instruction that consumes the value does (DenormalOperand(s)).
  float32_t v = {bits};
  extFloat80_t r;
  softfloat_exceptionFlags = 0;
  f32_to_extF80M(v, &r);
  fpu_end(f);
  return r;
}

extFloat80_t fpu_load_f64(x86_fpu* f, uint64_t bits) {
  float64_t v = {bits};
  extFloat80_t r;
  softfloat_exceptionFlags = 0;
  f64_to_extF80M(v, &r);
  fpu_end(f);
  return r;
}

extFloat80_t fpu_load_i16(x86_fpu* f, int16_t v) {
  extFloat80_t r;
  softfloat_exceptionFlags = 0;
  i32_to_extF80M(v, &r);
  fpu_end(f);
  return r;
}

extFloat80_t fpu_load_i32(x86_fpu* f, int32_t v) {
  extFloat80_t r;
  softfloat_exceptionFlags = 0;
  i32_to_extF80M(v, &r);
  fpu_end(f);
  return r;
}

extFloat80_t fpu_load_i64(x86_fpu* f, int64_t v) {
  extFloat80_t r;
  softfloat_exceptionFlags = 0;
  i64_to_extF80M(v, &r);
  fpu_end(f);
  return r;
}

uint32_t fpu_store_f32(x86_fpu* f, extFloat80_t v) {
  fpu_begin(f);
  float32_t r = extF80M_to_f32(&v);
  fpu_end(f);
  return r.v;
}

uint64_t fpu_store_f64(x86_fpu* f, extFloat80_t v) {
  fpu_begin(f);
  float64_t r = extF80M_to_f64(&v);
  fpu_end(f);
  return r.v;
}

int16_t fpu_store_i16(x86_fpu* f, extFloat80_t v) {
  fpu_begin(f);
  int32_t q = (int32_t)extF80M_to_i32(&v, softfloat_roundingMode, true);
  int bad = q < -32768 || q > 32767;
  fpu_end(f);
  if (bad) {
    f->sw |= kFpuIe;
    UpdateEs(f);
    return (int16_t)0x8000;
  }
  return (int16_t)q;
}

int32_t fpu_store_i32(x86_fpu* f, extFloat80_t v) {
  fpu_begin(f);
  int32_t q = (int32_t)extF80M_to_i32(&v, softfloat_roundingMode, true);
  fpu_end(f);
  return q;
}

int64_t fpu_store_i64(x86_fpu* f, extFloat80_t v) {
  fpu_begin(f);
  int64_t q = (int64_t)extF80M_to_i64(&v, softfloat_roundingMode, true);
  fpu_end(f);
  return q;
}

int16_t fpu_store_i16_trunc(x86_fpu* f, extFloat80_t v) {
  fpu_begin(f);
  int32_t q = (int32_t)extF80M_to_i32_r_minMag(&v, true);
  int bad = q < -32768 || q > 32767;
  fpu_end(f);
  if (bad) {
    f->sw |= kFpuIe;
    UpdateEs(f);
    return (int16_t)0x8000;
  }
  return (int16_t)q;
}

int32_t fpu_store_i32_trunc(x86_fpu* f, extFloat80_t v) {
  fpu_begin(f);
  int32_t q = (int32_t)extF80M_to_i32_r_minMag(&v, true);
  fpu_end(f);
  return q;
}

int64_t fpu_store_i64_trunc(x86_fpu* f, extFloat80_t v) {
  fpu_begin(f);
  int64_t q = (int64_t)extF80M_to_i64_r_minMag(&v, true);
  fpu_end(f);
  return q;
}

extFloat80_t fpu_load_bcd(x86_fpu* f, const uint8_t bcd[10]) {
  // 18 packed digits: byte 8 holds the most significant pair, byte 0 the
  // least significant; byte 9's bit 7 is the sign (SDM FBLD; QEMU
  // helper_fbld_ST0 iterates i = 8 down to 0).
  uint64_t v = 0;
  for (int i = 8; i >= 0; i--) {
    v = v * 100 + (uint64_t)((bcd[i] >> 4) & 0xf) * 10 + (bcd[i] & 0xf);
  }
  extFloat80_t r;
  softfloat_exceptionFlags = 0;
  ui64_to_extF80M(v, &r);
  if (bcd[9] & 0x80) r.signExp ^= 0x8000;
  fpu_end(f);
  return r;
}

void fpu_store_bcd(x86_fpu* f, extFloat80_t v, uint8_t out[10]) {
  fpu_begin(f);
  int64_t q = (int64_t)extF80M_to_i64(&v, softfloat_roundingMode, true);
  fpu_end(f);
  int neg = q < 0;
  uint64_t mag = q == INT64_MIN ? 0 : (uint64_t)(neg ? -q : q);
  if (q == INT64_MIN || mag >= 1000000000000000000ULL) {
    // Integer indefinite BCD image (QEMU helper_fbst_ST0).
    f->sw |= kFpuIe;
    memset(out, 0, 10);
    out[7] = 0xc0;
    out[8] = 0xff;
    out[9] = 0xff;
    UpdateEs(f);
    return;
  }
  memset(out, 0, 10);
  for (int i = 0; i < 9 && mag; i++) {
    uint32_t pair = (uint32_t)(mag % 100);
    mag /= 100;
    out[i] = (uint8_t)(((pair / 10) << 4) | (pair % 10));
  }
  out[9] = neg ? 0x80 : 0x00;
  UpdateEs(f);
}

// ---- environment / state images ---------------------------------------------

static void Put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static uint16_t Get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void Put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
static uint32_t Get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void fpu_store_env(const x86_fpu* f, uint8_t* out, int wide, int real) {
  memset(out, 0, wide ? 28 : 14);
  if (wide) {
    // The 32-bit environment stores each control/status/tag word in a 32-bit
    // slot with the upper half reserved; QEMU/Bochs fill it with ones.
    Put32(out + 0, 0xffff0000u | f->cw);
    Put32(out + 4, 0xffff0000u | f->sw);
    Put32(out + 8, 0xffff0000u | f->tw);
    if (real) {
      // Real-address mode/V86 (SDM vol.1 figure 8-10): FIP and FDP are 20-bit
      // linear addresses packed as low 16 bits at +0C/+14 and bits 31:16 in
      // the high half of the dword at +10/+18 (bits 27:12); the opcode shares
      // that dword's low 11 bits. No FCS/FDS fields exist. Bochs fpu.cc
      // fpu_save_environment computes (FCS << 4) + FIP the same way.
      uint32_t fip = ((uint32_t)f->fcs << 4) + f->fip;
      uint32_t fdp = ((uint32_t)f->fds << 4) + f->fdp;
      Put32(out + 12, 0xffff0000u | (fip & 0xffffu));
      Put32(out + 16, ((fip >> 16) << 12) | (f->fop & 0x07ffu));
      Put32(out + 20, 0xffff0000u | (fdp & 0xffffu));
      Put32(out + 24, (fdp >> 16) << 12);
    } else {
      Put32(out + 12, f->fip);
      Put16(out + 16, f->fcs);
      Put16(out + 18, f->fop & 0x07ffu);
      Put32(out + 20, f->fdp);
      Put16(out + 24, f->fds);
    }
  } else {
    Put16(out + 0, f->cw);
    Put16(out + 2, f->sw);
    Put16(out + 4, f->tw);
    if (real) {
      // Real-address mode/V86, 16-bit operand size (SDM figure 8-12): the
      // 20-bit linear address is low 16 bits at +6/+10 and bits 19:16 in
      // bits 15:12 of +8/+12; the opcode sits in bits 10:0 of +8.
      uint32_t fip = ((uint32_t)f->fcs << 4) + f->fip;
      uint32_t fdp = ((uint32_t)f->fds << 4) + f->fdp;
      Put16(out + 6, (uint16_t)fip);
      Put16(out + 8, (uint16_t)(((fip >> 16) & 0xfu) << 12) | (f->fop & 0x07ffu));
      Put16(out + 10, (uint16_t)fdp);
      Put16(out + 12, (uint16_t)(((fdp >> 16) & 0xfu) << 12));
    } else {
      Put16(out + 6, (uint16_t)f->fip);
      Put16(out + 8, f->fcs);
      Put16(out + 10, (uint16_t)f->fdp);
      Put16(out + 12, f->fds);
    }
  }
}

void fpu_load_env(x86_fpu* f, const uint8_t* in, int wide, int real) {
  if (wide) {
    f->cw = Get16(in + 0);
    f->sw = Get16(in + 4);
    f->tw = Get16(in + 8);
    if (real) {
      // FCS/FDS are cleared outside protected mode (SDM vol.1 8.1.7); FIP and
      // FDP are the 20-bit linear addresses from the real-mode image.
      f->fip = Get16(in + 12) | ((Get32(in + 16) & 0x0ffff000u) << 4);
      f->fop = Get32(in + 16) & 0x07ffu;
      f->fdp = Get16(in + 20) | ((Get32(in + 24) & 0x0ffff000u) << 4);
      f->fcs = 0;
      f->fds = 0;
    } else {
      f->fip = Get32(in + 12);
      f->fcs = Get16(in + 16);
      f->fop = Get16(in + 18) & 0x07ffu;
      f->fdp = Get32(in + 20);
      f->fds = Get16(in + 24);
    }
  } else {
    f->cw = Get16(in + 0);
    f->sw = Get16(in + 2);
    f->tw = Get16(in + 4);
    if (real) {
      f->fip = Get16(in + 6) | ((Get16(in + 8) & 0xf000u) << 4);
      f->fop = Get16(in + 8) & 0x07ffu;
      f->fdp = Get16(in + 10) | ((Get16(in + 12) & 0xf000u) << 4);
      f->fcs = 0;
      f->fds = 0;
    } else {
      f->fip = Get16(in + 6);
      f->fcs = Get16(in + 8);
      f->fdp = Get16(in + 10);
      f->fds = Get16(in + 12);
    }
  }
  // FLDENV loads the tag word, then the non-empty entries take their class
  // from the register they describe. Bochs fpu.cc FLDENV loops over the
  // loaded tags and calls FPU_settagi(FPU_tagof(reg)) for every non-empty
  // one; QEMU's do_fldenv keeps only the empty bit and classifies at save
  // time. Preserving the loaded empty bits and recomputing the class is the
  // common behaviour. The control word gets the same reserved-bit
  // normalization as FLDCW.
  fpu_load_cw(f, f->cw);
  for (int i = 0; i < 8; i++)
    if (fpu_tag(f, i) != kFpuTagEmpty) fpu_mark(f, i);
  UpdateEs(f);
}

void fpu_store_state(const x86_fpu* f, uint8_t* out, int wide, int real) {
  int n = wide ? 28 : 14;
  fpu_store_env(f, out, wide, real);
  for (int i = 0; i < 8; i++) {
    extFloat80_t v = f->st[(fpu_top(f) + i) & 7];
    Put32(out + n + i * 10, (uint32_t)v.signif);
    Put32(out + n + i * 10 + 4, (uint32_t)(v.signif >> 32));
    Put16(out + n + i * 10 + 8, v.signExp);
  }
}

void fpu_load_state(x86_fpu* f, const uint8_t* in, int wide, int real) {
  int n = wide ? 28 : 14;
  // FRSTOR loads the tags before the register images; the images are in
  // stack order (ST0 first), unlike the tag word's physical order. Once the
  // registers are in place, a non-empty tag takes its class from the value
  // it names (Bochs fpu.cc FRSTOR: "update tag only if it is not empty").
  uint16_t tw = wide ? Get16(in + 8) : Get16(in + 4);
  fpu_load_env(f, in, wide, real);
  for (int i = 0; i < 8; i++) {
    extFloat80_t v;
    v.signif = Get32(in + n + i * 10) | ((uint64_t)Get32(in + n + i * 10 + 4) << 32);
    v.signExp = Get16(in + n + i * 10 + 8);
    f->st[(fpu_top(f) + i) & 7] = v;
  }
  f->tw = tw;
  for (int i = 0; i < 8; i++)
    if (fpu_tag(f, i) != kFpuTagEmpty) fpu_mark(f, i);
  UpdateEs(f);
}

void fpu_fxsave(const x86_fpu* f, uint8_t* out) {
  memset(out, 0, 512);
  Put16(out + 0, f->cw);
  Put16(out + 2, f->sw);
  // The FXSAVE tag word is the abridged one: one bit per register, 0 = empty
  // and 1 = in use (SDM Table 10-1; QEMU do_xsave_fpu writes fptag ^ 0xff).
  uint16_t tw = 0;
  for (int i = 0; i < 8; i++)
    if (fpu_tag(f, i) != kFpuTagEmpty) tw |= (uint16_t)(1u << i);
  Put16(out + 4, tw);
  Put16(out + 6, f->fop);
  Put32(out + 8, f->fip);
  Put16(out + 12, f->fcs);
  Put32(out + 16, f->fdp);
  Put16(out + 20, f->fds);
  Put32(out + 24, f->mxcsr);
  Put32(out + 28, f->mxcsr_mask);
  for (int i = 0; i < 8; i++) {
    extFloat80_t v = f->st[(fpu_top(f) + i) & 7];
    memcpy(out + 32 + i * 16, &v.signif, 8);
    Put16(out + 32 + i * 16 + 8, v.signExp);
  }
  for (int i = 0; i < 8; i++) memcpy(out + 160 + i * 16, f->xmm[i], 16);
}

void fpu_sse_begin(x86_fpu* f) {
  // MXCSR.RC (bits 14:13): 00 nearest, 01 down, 10 up, 11 truncate (SDM
  // vol.1 10.2.3, figure 10-3).
  static const uint_fast8_t kRc[4] = {softfloat_round_near_even, softfloat_round_min,
                                     softfloat_round_max, softfloat_round_minMag};
  softfloat_roundingMode = kRc[(f->mxcsr >> 13) & 3];
  softfloat_exceptionFlags = 0;
}

int fpu_sse_end(x86_fpu* f) {
  uint_fast8_t sf = softfloat_exceptionFlags;
  uint32_t st = 0;
  if (sf & softfloat_flag_invalid) st |= 1u << 0;
  if (sf & softfloat_flag_infinite) st |= 1u << 2;
  if (sf & softfloat_flag_overflow) st |= 1u << 3;
  if (sf & softfloat_flag_underflow) st |= 1u << 4;
  if (sf & softfloat_flag_inexact) st |= 1u << 5;
  softfloat_exceptionFlags = 0;
  f->mxcsr |= st;
  uint32_t raised = f->mxcsr & 0x3f;
  uint32_t masks = (f->mxcsr >> 7) & 0x3f;
  return (raised & ~masks) != 0;
}

int fpu_sse_daz(const x86_fpu* f) { return (f->mxcsr & (1u << 6)) != 0; }
int fpu_sse_ftz(const x86_fpu* f) { return (f->mxcsr & (1u << 15)) != 0; }

void fpu_sse_flush(uint8_t* elem, int bytes) {
  if (bytes == 4) {
    uint32_t v;
    memcpy(&v, elem, 4);
    if ((v & 0x7f800000u) == 0 && (v & 0x007fffffu) != 0) {
      v &= 0x80000000u;
      memcpy(elem, &v, 4);
    }
  } else if (bytes == 8) {
    uint64_t v;
    memcpy(&v, elem, 8);
    if ((v & 0x7ff0000000000000ULL) == 0 && (v & 0x000fffffffffffffULL) != 0) {
      v &= 0x8000000000000000ULL;
      memcpy(elem, &v, 8);
    }
  }
}

void fpu_fxrstor(x86_fpu* f, const uint8_t* in) {
  fpu_load_cw(f, Get16(in + 0));
  f->sw = Get16(in + 2);
  f->fop = Get16(in + 6);
  f->fip = Get32(in + 8);
  f->fcs = Get16(in + 12);
  f->fdp = Get32(in + 16);
  f->fds = Get16(in + 20);
  f->mxcsr = Get32(in + 24);
  for (int i = 0; i < 8; i++) {
    extFloat80_t v;
    memcpy(&v.signif, in + 32 + i * 16, 8);
    v.signExp = Get16(in + 32 + i * 16 + 8);
    f->st[(fpu_top(f) + i) & 7] = v;
  }
  // The abridged bits are physical (QEMU keeps fptags by register number);
  // an in-use register's zero/special/valid class comes from its value.
  uint16_t tw = 0;
  for (int i = 0; i < 8; i++)
    tw |= (uint16_t)(((in[4] & (1u << i)) ? TagOf(f->st[i]) : kFpuTagEmpty) << (2 * i));
  f->tw = tw;
  for (int i = 0; i < 8; i++) memcpy(f->xmm[i], in + 160 + i * 16, 16);
  UpdateEs(f);
}
