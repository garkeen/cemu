// IEEE-754 operations for F and D extensions. Host double/float provide RNE
// results; long double (x87 80-bit) provides the exact value so that inexact
// detection and directed rounding modes are correct.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "host/host.h"
#include "riscv.h"
#include "util/log.h"

static const uint64_t kBoxMask = 0xffffffff00000000ULL;
static const uint64_t kQnan64 = 0x7ff8000000000000ULL;
static const uint32_t kQnan32 = 0x7fc00000u;

static uint64_t Box32(uint32_t v) { return kBoxMask | v; }

static int IsBoxed(uint64_t v) { return (v >> 32) == 0xffffffffULL; }

static uint32_t Bits32(float f) {
  uint32_t v;
  memcpy(&v, &f, 4);
  return v;
}

static float Float32(uint64_t v) {
  uint32_t lo = (uint32_t)v;
  float f;
  memcpy(&f, &lo, 4);
  return f;
}

static uint64_t Bits64(double d) {
  uint64_t v;
  memcpy(&v, &d, 8);
  return v;
}

static double Float64(uint64_t v) {
  double d;
  memcpy(&d, &v, 8);
  return d;
}

static int IsSnanF(float f) {
  uint32_t v = Bits32(f);
  return ((v >> 23) & 0xff) == 0xff && (v & 0x7fffff) != 0 && !(v & 0x400000);
}

static int IsSnanD(double d) {
  uint64_t v = Bits64(d);
  return ((v >> 52) & 0x7ff) == 0x7ff && (v & 0xfffffffffffffULL) != 0 && !(v & (1ULL << 51));
}

static float MinNormalF(void) {
  uint32_t v = 0x00800000u;
  float f;
  memcpy(&f, &v, 4);
  return f;
}

static void RoundSAdjust(RiscvState* s, float* r, long double e, int rm) {
  if ((long double)*r == e) return;
  s->fflags |= kFflagNX;
  switch (rm) {
    case kRmRne:
      break;
    case kRmRup:
      if ((long double)*r < e) *r = nextafterf(*r, INFINITY);
      break;
    case kRmRdn:
      if ((long double)*r > e) *r = nextafterf(*r, -INFINITY);
      break;
    case kRmRtz:
      if ((*r > 0 && (long double)*r > e) || (*r < 0 && (long double)*r < e))
        *r = nextafterf(*r, *r > 0 ? -INFINITY : INFINITY);
      break;
    case kRmRmm: {
      float toward = nextafterf(*r, (float)e);
      if (fabsl(e - (long double)toward) == fabsl(e - (long double)*r)) {
        if (fabsl((long double)toward) > fabsl((long double)*r)) *r = toward;
      }
      break;
    }
    default:
      break;
  }
}

static void RoundDAdjust(RiscvState* s, double* r, long double e, int rm) {
  if ((long double)*r == e) return;
  s->fflags |= kFflagNX;
  switch (rm) {
    case kRmRne:
      break;
    case kRmRup:
      if ((long double)*r < e) *r = nextafter(*r, INFINITY);
      break;
    case kRmRdn:
      if ((long double)*r > e) *r = nextafter(*r, -INFINITY);
      break;
    case kRmRtz:
      if ((*r > 0 && (long double)*r > e) || (*r < 0 && (long double)*r < e))
        *r = nextafter(*r, *r > 0 ? -INFINITY : INFINITY);
      break;
    case kRmRmm: {
      double toward = nextafter(*r, (double)e);
      if (fabsl(e - (long double)toward) == fabsl(e - (long double)*r)) {
        if (fabsl((long double)toward) > fabsl((long double)*r)) *r = toward;
      }
      break;
    }
    default:
      break;
  }
}

static uint32_t OverflowResult32(int negative, int rm) {
  uint32_t inf = negative ? 0xff800000u : 0x7f800000u;
  uint32_t max = negative ? 0xff7fffffu : 0x7f7fffffu;
  switch (rm) {
    case kRmRne:
    case kRmRmm:
      return inf;
    case kRmRtz:
      return max;
    case kRmRup:
      return negative ? max : inf;
    case kRmRdn:
      return negative ? inf : max;
    default:
      return inf;
  }
}

static uint64_t OverflowResult64(int negative, int rm) {
  uint64_t inf = negative ? 0xfff0000000000000ULL : 0x7ff0000000000000ULL;
  uint64_t max = negative ? 0xffefffffffffffffULL : 0x7fefffffffffffffULL;
  switch (rm) {
    case kRmRne:
    case kRmRmm:
      return inf;
    case kRmRtz:
      return max;
    case kRmRup:
      return negative ? max : inf;
    case kRmRdn:
      return negative ? inf : max;
    default:
      return inf;
  }
}

static uint64_t FinishS(RiscvState* s, float r, long double e, int rm) {
  if (isnan(r)) {
    s->fflags |= kFflagNV;
    return Box32(kQnan32);
  }
  if (isinf(r) && !isinf((double)e)) {
    s->fflags |= kFflagOF | kFflagNX;
    return Box32(OverflowResult32(r < 0, rm));
  }
  if (e != 0 && fabsl(e) < (long double)MinNormalF()) {
    if ((long double)r != e) s->fflags |= kFflagUF | kFflagNX;
  }
  RoundSAdjust(s, &r, e, rm);
  return Box32(Bits32(r));
}

static uint64_t FinishD(RiscvState* s, double r, long double e, int rm) {
  if (isnan(r)) {
    s->fflags |= kFflagNV;
    return kQnan64;
  }
  if (isinf(r) && !isinf(e)) {
    s->fflags |= kFflagOF | kFflagNX;
    return OverflowResult64(r < 0, rm);
  }
  if (e != 0 && fabsl(e) < 2.2250738585072014e-308L) {
    if ((long double)r != e) s->fflags |= kFflagUF | kFflagNX;
  }
  RoundDAdjust(s, &r, e, rm);
  return Bits64(r);
}

uint64_t RiscvFpBinaryS(RiscvState* s, uint64_t ra, uint64_t rb, int kind, int rm) {
  if (!IsBoxed(ra) || !IsBoxed(rb)) {
    s->fflags |= kFflagNV;
    return Box32(kQnan32);
  }
  float a = Float32(ra), b = Float32(rb);
  if (IsSnanF(a) || IsSnanF(b)) s->fflags |= kFflagNV;
  float r;
  long double e;
  switch (kind) {
    case kFpAdd:
      r = a + b;
      e = (long double)a + (long double)b;
      break;
    case kFpSub:
      r = a - b;
      e = (long double)a - (long double)b;
      break;
    case kFpMul:
      r = a * b;
      e = (long double)a * (long double)b;
      break;
    case kFpDiv:
      if (b == 0 && isfinite(a) && a != 0) {
        s->fflags |= kFflagDZ;
        r = a / b;
        return Box32(Bits32(r));
      }
      r = a / b;
      e = (long double)a / (long double)b;
      return FinishS(s, r, e, rm);
    default:
      Fatal("fp: bad kind %d", kind);
      return 0;
  }
  return FinishS(s, r, e, rm);
}

uint64_t RiscvFpBinaryD(RiscvState* s, uint64_t ra, uint64_t rb, int kind, int rm) {
  double a = Float64(ra), b = Float64(rb);
  if (IsSnanD(a) || IsSnanD(b)) s->fflags |= kFflagNV;
  double r;
  long double e;
  switch (kind) {
    case kFpAdd:
      r = a + b;
      e = (long double)a + (long double)b;
      break;
    case kFpSub:
      r = a - b;
      e = (long double)a - (long double)b;
      break;
    case kFpMul:
      r = a * b;
      e = (long double)a * (long double)b;
      break;
    case kFpDiv:
      if (b == 0 && isfinite(a) && a != 0) {
        s->fflags |= kFflagDZ;
        r = a / b;
        return Bits64(r);
      }
      r = a / b;
      e = (long double)a / (long double)b;
      return FinishD(s, r, e, rm);
    default:
      Fatal("fp: bad kind %d", kind);
      return 0;
  }
  return FinishD(s, r, e, rm);
}

uint64_t RiscvFpSqrtS(RiscvState* s, uint64_t ra, int rm) {
  if (!IsBoxed(ra)) {
    s->fflags |= kFflagNV;
    return Box32(kQnan32);
  }
  float a = Float32(ra);
  if (IsSnanF(a)) s->fflags |= kFflagNV;
  if (a < 0 && !isnan(a)) {
    s->fflags |= kFflagNV;
    return Box32(kQnan32);
  }
  float r = sqrtf(a);
  long double e = sqrtl((long double)a);
  return FinishS(s, r, e, rm);
}

uint64_t RiscvFpSqrtD(RiscvState* s, uint64_t ra, int rm) {
  double a = Float64(ra);
  if (IsSnanD(a)) s->fflags |= kFflagNV;
  if (a < 0 && !isnan(a)) {
    s->fflags |= kFflagNV;
    return kQnan64;
  }
  double r = sqrt(a);
  long double e = sqrtl((long double)a);
  return FinishD(s, r, e, rm);
}

uint64_t RiscvFpFmaS(RiscvState* s, uint64_t ra, uint64_t rb, uint64_t rc, int rm) {
  if (!IsBoxed(ra) || !IsBoxed(rb) || !IsBoxed(rc)) {
    s->fflags |= kFflagNV;
    return Box32(kQnan32);
  }
  float a = Float32(ra), b = Float32(rb), c = Float32(rc);
  if (IsSnanF(a) || IsSnanF(b) || IsSnanF(c)) s->fflags |= kFflagNV;
  float r = fmaf(a, b, c);
  long double e = (long double)a * (long double)b + (long double)c;
  return FinishS(s, r, e, rm);
}

uint64_t RiscvFpFmaD(RiscvState* s, uint64_t ra, uint64_t rb, uint64_t rc, int rm) {
  double a = Float64(ra), b = Float64(rb), c = Float64(rc);
  if (IsSnanD(a) || IsSnanD(b) || IsSnanD(c)) s->fflags |= kFflagNV;
  double r = fma(a, b, c);
  long double e = (long double)a * (long double)b + (long double)c;
  return FinishD(s, r, e, rm);
}

// fmin/fmax: NaN inputs yield the other operand; sNaN raises NV. An unboxed
// single-precision input is treated as a canonical NaN.
static uint64_t MinMaxS(RiscvState* s, uint64_t ra, uint64_t rb, int is_max) {
  float a = IsBoxed(ra) ? Float32(ra) : Float32(Box32(kQnan32));
  float b = IsBoxed(rb) ? Float32(rb) : Float32(Box32(kQnan32));
  if (IsSnanF(Float32(ra)) || IsSnanF(Float32(rb))) s->fflags |= kFflagNV;
  if (isnan(a) && !isnan(b)) return Box32(Bits32(b));
  if (!isnan(a) && isnan(b)) return Box32(Bits32(a));
  if (isnan(a) && isnan(b)) return Box32(kQnan32);
  if (a == 0 && b == 0) return Box32(Bits32(is_max ? +0.0f : -0.0f));
  float r = is_max ? (a > b ? a : b) : (a < b ? a : b);
  return Box32(Bits32(r));
}

static uint64_t MinMaxD(RiscvState* s, uint64_t ra, uint64_t rb, int is_max) {
  double a = Float64(ra), b = Float64(rb);
  if (IsSnanD(a) || IsSnanD(b)) s->fflags |= kFflagNV;
  if (isnan(a) && !isnan(b)) return Bits64(b);
  if (!isnan(a) && isnan(b)) return Bits64(a);
  if (isnan(a) && isnan(b)) return kQnan64;
  if (a == 0 && b == 0) return Bits64(is_max ? +0.0 : -0.0);
  double r = is_max ? (a > b ? a : b) : (a < b ? a : b);
  return Bits64(r);
}

uint64_t RiscvFpMinMaxS(RiscvState* s, uint64_t ra, uint64_t rb, int is_max) {
  return MinMaxS(s, ra, rb, is_max);
}

uint64_t RiscvFpMinMaxD(RiscvState* s, uint64_t ra, uint64_t rb, int is_max) {
  return MinMaxD(s, ra, rb, is_max);
}

uint64_t RiscvFpCmpS(RiscvState* s, uint64_t ra, uint64_t rb, int kind) {
  if (!IsBoxed(ra) || !IsBoxed(rb)) {
    s->fflags |= kFflagNV;
    return 0;
  }
  float a = Float32(ra), b = Float32(rb);
  if (isnan(a) || isnan(b)) {
    if (kind != kFpEq || IsSnanF(a) || IsSnanF(b)) s->fflags |= kFflagNV;
    return 0;
  }
  switch (kind) {
    case kFpEq:
      return a == b;
    case kFpLt:
      return a < b;
    case kFpLe:
      return a <= b;
    default:
      Fatal("fp: bad cmp kind %d", kind);
      return 0;
  }
}

uint64_t RiscvFpCmpD(RiscvState* s, uint64_t ra, uint64_t rb, int kind) {
  double a = Float64(ra), b = Float64(rb);
  if (isnan(a) || isnan(b)) {
    if (kind != kFpEq || IsSnanD(a) || IsSnanD(b)) s->fflags |= kFflagNV;
    return 0;
  }
  switch (kind) {
    case kFpEq:
      return a == b;
    case kFpLt:
      return a < b;
    case kFpLe:
      return a <= b;
    default:
      Fatal("fp: bad cmp kind %d", kind);
      return 0;
  }
}

static int Classify32(uint32_t v) {
  int exp = (int)((v >> 23) & 0xff);
  uint32_t frac = v & 0x7fffff;
  int sign = (int)(v >> 31);
  if (exp == 0xff) {
    if (frac == 0) return sign ? 0 : 7;  // -inf / +inf
    return (v & 0x400000) ? 9 : 8;       // qNaN / sNaN
  }
  if (exp == 0) {
    if (frac == 0) return sign ? 3 : 4;  // -zero / +zero
    return sign ? 2 : 5;                 // subnormal
  }
  return sign ? 1 : 6;  // normal
}

uint64_t RiscvFpClassS(uint64_t ra) { return 1ULL << Classify32((uint32_t)ra); }

static int Classify64(uint64_t v) {
  int exp = (int)((v >> 52) & 0x7ff);
  uint64_t frac = v & 0xfffffffffffffULL;
  int sign = (int)(v >> 63);
  if (exp == 0x7ff) {
    if (frac == 0) return sign ? 0 : 7;
    return (v & (1ULL << 51)) ? 9 : 8;
  }
  if (exp == 0) {
    if (frac == 0) return sign ? 3 : 4;
    return sign ? 2 : 5;
  }
  return sign ? 1 : 6;
}

uint64_t RiscvFpClassD(uint64_t ra) { return 1ULL << Classify64(ra); }

static uint64_t Sgnj64(uint64_t a, uint64_t b, int kind) {
  uint64_t sa = a & 0x8000000000000000ULL;
  uint64_t sb = b & 0x8000000000000000ULL;
  uint64_t rs;
  if (kind == 0)
    rs = sb;
  else if (kind == 1)
    rs = sb ^ 0x8000000000000000ULL;
  else
    rs = sa ^ sb;
  return (a & ~0x8000000000000000ULL) | rs;
}

// fsgnj.s: an unboxed rs1 is replaced by the canonical NaN; an unboxed rs2
// contributes sign bit 0.
uint64_t RiscvFpSgnjS(uint64_t ra, uint64_t rb, int kind) {
  uint32_t a = IsBoxed(ra) ? (uint32_t)ra : kQnan32;
  uint32_t sb = (IsBoxed(rb) && ((uint32_t)rb >> 31)) ? 1u : 0u;
  uint32_t sa = a >> 31;
  uint32_t rs;
  if (kind == 0)
    rs = sb ? 0x80000000u : 0;  // fsgnj
  else if (kind == 1)
    rs = sb ? 0 : 0x80000000u;  // fsgnjn
  else
    rs = (sa ^ sb) ? 0x80000000u : 0;  // fsgnjx
  return Box32((a & ~0x80000000u) | rs);
}

uint64_t RiscvFpSgnjD(uint64_t ra, uint64_t rb, int kind) { return Sgnj64(ra, rb, kind); }

static long double F2IExact(long double e, int to_signed, int dbits, int* ok) {
  long double max = to_signed ? (dbits == 32 ? 2147483647.0L : 9223372036854775807.0L)
                              : (dbits == 32 ? 4294967295.0L : 18446744073709551615.0L);
  long double min = to_signed ? (dbits == 32 ? -2147483648.0L : -9223372036854775808.0L) : 0.0L;
  if (e > max) {
    *ok = 0;
    return max;
  }
  if (e < min) {
    // values in (-1, 0) round to zero and are valid for unsigned targets
    if (!to_signed && e > -1.0L) {
      *ok = 1;
      return 0;
    }
    *ok = 0;
    return min;
  }
  *ok = 1;
  return e;
}

static uint64_t F2IResult(RiscvState* s, long double e, int to_signed, int dbits, int rm) {
  if (isnan(e)) {
    // NaN converts like positive overflow: the most positive value
    s->fflags |= kFflagNV;
    return to_signed ? (dbits == 32 ? 0x7fffffffu : 0x7fffffffffffffffULL) : 0xffffffffffffffffULL;
  }
  long double cand;
  switch (rm) {
    case kRmRtz:
      cand = truncl(e);
      break;
    case kRmRdn:
      cand = floorl(e);
      break;
    case kRmRup:
      cand = ceill(e);
      break;
    case kRmRmm:
      cand = roundl(e);
      break;
    default:
      cand = nearbyintl(e);
      break;  // RNE
  }
  int neg = cand < 0;
  int ok;
  long double clamped = F2IExact(cand, to_signed, dbits, &ok);
  if (!ok) {
    s->fflags |= kFflagNV;
    if (to_signed)
      return neg ? (dbits == 32 ? 0xffffffff80000000ULL : 0x8000000000000000ULL)
                 : (dbits == 32 ? 0x7fffffffu : 0x7fffffffffffffffULL);
    // unsigned negative overflow clamps to 0, positive to the max
    return neg ? 0 : 0xffffffffffffffffULL;
  }
  if (clamped != e) s->fflags |= kFflagNX;
  // RV64: both signed and unsigned 32-bit results are sign-extended into rd
  if (to_signed) return dbits == 32 ? (uint64_t)(int32_t)clamped : (uint64_t)(int64_t)clamped;
  return dbits == 32 ? (uint64_t)(int64_t)(int32_t)(uint32_t)clamped : (uint64_t)clamped;
}

uint64_t RiscvFpF2IS(RiscvState* s, uint64_t ra, int to_signed, int dbits, int rm) {
  if (!IsBoxed(ra)) {
    s->fflags |= kFflagNV;
    return to_signed ? 0x8000000000000000ULL : 0xffffffffffffffffULL;
  }
  long double e = (long double)Float32(ra);
  return F2IResult(s, e, to_signed, dbits, rm);
}

uint64_t RiscvFpF2ID(RiscvState* s, uint64_t ra, int to_signed, int dbits, int rm) {
  long double e = (long double)Float64(ra);
  return F2IResult(s, e, to_signed, dbits, rm);
}

static uint64_t I2FCommonS(RiscvState* s, long double e, int rm) {
  float r = (float)e;
  return FinishS(s, r, e, rm);
}

static uint64_t I2FCommonD(RiscvState* s, long double e, int rm) {
  double r = (double)e;
  return FinishD(s, r, e, rm);
}

uint64_t RiscvFpI2FS(RiscvState* s, uint64_t val, int val_signed, int bits, int rm) {
  long double e;
  if (val_signed)
    e = (bits == 32) ? (long double)(int32_t)val : (long double)(int64_t)val;
  else
    e = (bits == 32) ? (long double)(uint32_t)val : (long double)(uint64_t)val;
  return I2FCommonS(s, e, rm);
}

uint64_t RiscvFpI2FD(RiscvState* s, uint64_t val, int val_signed, int bits, int rm) {
  long double e;
  if (val_signed)
    e = (bits == 32) ? (long double)(int32_t)val : (long double)(int64_t)val;
  else
    e = (bits == 32) ? (long double)(uint32_t)val : (long double)(uint64_t)val;
  return I2FCommonD(s, e, rm);
}

uint64_t RiscvFpCvtSD(RiscvState* s, uint64_t ra, int rm) {
  // the input is a double: NaN boxing does not apply to D values
  long double e = (long double)Float64(ra);
  float r = (float)e;
  return FinishS(s, r, e, rm);
}

uint64_t RiscvFpCvtDS(RiscvState* s, uint64_t ra, int rm) {
  if (!IsBoxed(ra)) {
    s->fflags |= kFflagNV;
    return kQnan64;
  }
  long double e = (long double)Float32(ra);
  double r = (double)e;
  return FinishD(s, r, e, rm);
}
