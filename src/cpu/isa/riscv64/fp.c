// IEEE-754 operations for the F and D extensions, executed by Berkeley
// SoftFloat 3e (the tree's shared cpu/softfloat library, also backing x87).
// SoftFloat gives bit-exact results for every rounding mode, subnormal, NaN and
// exception-flag combination; the host FPU is no longer consulted.
//
// Reference: Berkeley SoftFloat 3e (BSD-3, cpu/softfloat/softfloat.c); RISC-V
// unprivileged spec ch. 11 (F) and 12 (D); QEMU target/riscv/fpu_helper.c uses
// the same package. NaN boxing is spec 11.3: an unboxed single is the canonical
// quiet NaN, so it does not raise NV by itself.
#include "cpu/softfloat/softfloat.h"

#include "riscv.h"
#include "util/log.h"

static const uint64_t kBoxMask = 0xffffffff00000000ULL;
static const uint64_t kQnan64 = 0x7ff8000000000000ULL;
static const uint32_t kQnan32 = 0x7fc00000u;

static uint64_t Box32(uint32_t v) { return kBoxMask | v; }
static int IsBoxed(uint64_t v) { return (v >> 32) == 0xffffffffULL; }

static int IsNan32(uint32_t v) { return (v & 0x7fffffffu) > 0x7f800000u; }
static int IsNan64(uint64_t v) {
  return (v & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL;
}

static float32_t F32(uint32_t v) {
  float32_t f;
  f.v = v;
  return f;
}

static float64_t F64(uint64_t v) {
  float64_t f;
  f.v = v;
  return f;
}

// A single-precision source register: an unboxed value is the canonical qNaN
// (unprivileged spec 11.3), not an invalid encoding.
static uint32_t SrcS(uint64_t v) { return IsBoxed(v) ? (uint32_t)v : kQnan32; }

// RISC-V rm -> SoftFloat rounding mode. rm 5..7 are reserved; the decoder does
// not reject them yet, so they keep the previous effective RNE.
static const uint_fast8_t kSfRound[8] = {
    softfloat_round_near_even, softfloat_round_minMag, softfloat_round_min,
    softfloat_round_max, softfloat_round_near_maxMag, softfloat_round_near_even,
    softfloat_round_near_even, softfloat_round_near_even};

static void BeginOp(int rm) {
  softfloat_roundingMode = kSfRound[rm & 7];
  softfloat_exceptionFlags = 0;
}

static void TakeFlags(RiscvState* s) {
  uint_fast8_t f = softfloat_exceptionFlags;
  softfloat_exceptionFlags = 0;
  if (f & softfloat_flag_invalid) s->fflags |= kFflagNV;
  if (f & softfloat_flag_infinite) s->fflags |= kFflagDZ;
  if (f & softfloat_flag_overflow) s->fflags |= kFflagOF;
  if (f & softfloat_flag_underflow) s->fflags |= kFflagUF;
  if (f & softfloat_flag_inexact) s->fflags |= kFflagNX;
}

// RISC-V produces the canonical NaN for every arithmetic NaN result; SoftFloat
// may preserve an input payload or use the 8086-SSE default NaN.
static uint32_t Canon32(uint32_t v) { return IsNan32(v) ? kQnan32 : v; }
static uint64_t Canon64(uint64_t v) { return IsNan64(v) ? kQnan64 : v; }

// The invalid-conversion results RISC-V requires. SoftFloat's 8086-SSE build
// returns the x86 integer indefinite for every invalid case, which is not the
// RISC-V saturating value.
static uint64_t InvalidToI32(uint64_t src, int src_is64, int to_signed) {
  int neg = src_is64 ? (int)(src >> 63) : (int)((uint32_t)src >> 31);
  int nan = src_is64 ? IsNan64(src) : IsNan32((uint32_t)src);
  if (to_signed) return (nan || !neg) ? 0x7fffffffu : 0xffffffff80000000ULL;
  return (nan || !neg) ? 0xffffffffffffffffULL : 0;
}

static uint64_t InvalidToI64(uint64_t src, int src_is64, int to_signed) {
  int neg = src_is64 ? (int)(src >> 63) : (int)((uint32_t)src >> 31);
  int nan = src_is64 ? IsNan64(src) : IsNan32((uint32_t)src);
  if (to_signed) return (nan || !neg) ? 0x7fffffffffffffffULL : 0x8000000000000000ULL;
  return (nan || !neg) ? 0xffffffffffffffffULL : 0;
}

static uint64_t F2I32S(RiscvState* s, float32_t a, int to_signed);
static uint64_t F2I64S(RiscvState* s, float32_t a, int to_signed);
static uint64_t F2I32D(RiscvState* s, float64_t a, int to_signed);
static uint64_t F2I64D(RiscvState* s, float64_t a, int to_signed);

uint64_t RiscvFpBinaryS(RiscvState* s, uint64_t ra, uint64_t rb, int kind, int rm) {
  float32_t a = F32(SrcS(ra)), b = F32(SrcS(rb));
  BeginOp(rm);
  float32_t r;
  switch (kind) {
    case kFpAdd:
      r = f32_add(a, b);
      break;
    case kFpSub:
      r = f32_sub(a, b);
      break;
    case kFpMul:
      r = f32_mul(a, b);
      break;
    case kFpDiv:
      r = f32_div(a, b);
      break;
    default:
      Fatal("fp: bad kind %d", kind);
      return 0;
  }
  TakeFlags(s);
  return Box32(Canon32(r.v));
}

uint64_t RiscvFpBinaryD(RiscvState* s, uint64_t ra, uint64_t rb, int kind, int rm) {
  float64_t a = F64(ra), b = F64(rb);
  BeginOp(rm);
  float64_t r;
  switch (kind) {
    case kFpAdd:
      r = f64_add(a, b);
      break;
    case kFpSub:
      r = f64_sub(a, b);
      break;
    case kFpMul:
      r = f64_mul(a, b);
      break;
    case kFpDiv:
      r = f64_div(a, b);
      break;
    default:
      Fatal("fp: bad kind %d", kind);
      return 0;
  }
  TakeFlags(s);
  return Canon64(r.v);
}

uint64_t RiscvFpSqrtS(RiscvState* s, uint64_t ra, int rm) {
  BeginOp(rm);
  float32_t r = f32_sqrt(F32(SrcS(ra)));
  TakeFlags(s);
  return Box32(Canon32(r.v));
}

uint64_t RiscvFpSqrtD(RiscvState* s, uint64_t ra, int rm) {
  BeginOp(rm);
  float64_t r = f64_sqrt(F64(ra));
  TakeFlags(s);
  return Canon64(r.v);
}

uint64_t RiscvFpFmaS(RiscvState* s, uint64_t ra, uint64_t rb, uint64_t rc, int rm) {
  BeginOp(rm);
  float32_t r = f32_mulAdd(F32(SrcS(ra)), F32(SrcS(rb)), F32(SrcS(rc)));
  TakeFlags(s);
  return Box32(Canon32(r.v));
}

uint64_t RiscvFpFmaD(RiscvState* s, uint64_t ra, uint64_t rb, uint64_t rc, int rm) {
  BeginOp(rm);
  float64_t r = f64_mulAdd(F64(ra), F64(rb), F64(rc));
  TakeFlags(s);
  return Canon64(r.v);
}

// fmin/fmax: a NaN input yields the other operand, both NaNs yield the
// canonical NaN, and -0/+0 order by the sign bit. sNaN raises NV; a quiet NaN
// does not.
static uint64_t MinMaxS(RiscvState* s, uint64_t ra, uint64_t rb, int is_max) {
  float32_t a = F32(SrcS(ra)), b = F32(SrcS(rb));
  if (f32_isSignalingNaN(a) || f32_isSignalingNaN(b)) s->fflags |= kFflagNV;
  int anan = IsNan32(a.v), bnan = IsNan32(b.v);
  if (anan && bnan) return Box32(kQnan32);
  if (anan) return Box32(b.v);
  if (bnan) return Box32(a.v);
  if ((a.v & 0x7fffffffu) == 0 && (b.v & 0x7fffffffu) == 0)
    return Box32(is_max ? 0u : 0x80000000u);
  BeginOp(kRmRne);  // quiet compare of non-NaN values sets no flag
  int lt = f32_lt_quiet(a, b);
  TakeFlags(s);
  return Box32(is_max ? (lt ? b.v : a.v) : (lt ? a.v : b.v));
}

static uint64_t MinMaxD(RiscvState* s, uint64_t ra, uint64_t rb, int is_max) {
  float64_t a = F64(ra), b = F64(rb);
  if (f64_isSignalingNaN(a) || f64_isSignalingNaN(b)) s->fflags |= kFflagNV;
  int anan = IsNan64(a.v), bnan = IsNan64(b.v);
  if (anan && bnan) return kQnan64;
  if (anan) return b.v;
  if (bnan) return a.v;
  if ((a.v & 0x7fffffffffffffffULL) == 0 && (b.v & 0x7fffffffffffffffULL) == 0)
    return is_max ? 0ULL : 0x8000000000000000ULL;
  BeginOp(kRmRne);
  int lt = f64_lt_quiet(a, b);
  TakeFlags(s);
  return is_max ? (lt ? b.v : a.v) : (lt ? a.v : b.v);
}

uint64_t RiscvFpMinMaxS(RiscvState* s, uint64_t ra, uint64_t rb, int is_max) {
  return MinMaxS(s, ra, rb, is_max);
}

uint64_t RiscvFpMinMaxD(RiscvState* s, uint64_t ra, uint64_t rb, int is_max) {
  return MinMaxD(s, ra, rb, is_max);
}

uint64_t RiscvFpCmpS(RiscvState* s, uint64_t ra, uint64_t rb, int kind) {
  float32_t a = F32(SrcS(ra)), b = F32(SrcS(rb));
  BeginOp(kRmRne);
  int r;
  switch (kind) {
    case kFpEq:
      r = f32_eq(a, b);  // qNaN is quiet; sNaN raises NV
      break;
    case kFpLt:
      r = f32_lt(a, b);  // any NaN raises NV
      break;
    case kFpLe:
      r = f32_le(a, b);
      break;
    default:
      Fatal("fp: bad cmp kind %d", kind);
      return 0;
  }
  TakeFlags(s);
  return (uint64_t)r;
}

uint64_t RiscvFpCmpD(RiscvState* s, uint64_t ra, uint64_t rb, int kind) {
  float64_t a = F64(ra), b = F64(rb);
  BeginOp(kRmRne);
  int r;
  switch (kind) {
    case kFpEq:
      r = f64_eq(a, b);
      break;
    case kFpLt:
      r = f64_lt(a, b);
      break;
    case kFpLe:
      r = f64_le(a, b);
      break;
    default:
      Fatal("fp: bad cmp kind %d", kind);
      return 0;
  }
  TakeFlags(s);
  return (uint64_t)r;
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

uint64_t RiscvFpClassS(uint64_t ra) { return 1ULL << Classify32(SrcS(ra)); }

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

// fsgnj.s: an unboxed rs1 is the canonical NaN; an unboxed rs2 contributes sign
// bit 0.
uint64_t RiscvFpSgnjS(uint64_t ra, uint64_t rb, int kind) {
  uint32_t a = SrcS(ra);
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

static uint64_t F2I32S(RiscvState* s, float32_t a, int to_signed) {
  if (to_signed) {
    int_fast32_t v = f32_to_i32(a, softfloat_roundingMode, true);
    int inv = (softfloat_exceptionFlags & softfloat_flag_invalid) != 0;
    uint64_t r = inv ? InvalidToI32(a.v, 0, 1) : (uint64_t)(int64_t)(int32_t)v;
    TakeFlags(s);
    return r;
  }
  uint_fast32_t v = f32_to_ui32(a, softfloat_roundingMode, true);
  int inv = (softfloat_exceptionFlags & softfloat_flag_invalid) != 0;
  uint64_t r = inv ? InvalidToI32(a.v, 0, 0) : (uint64_t)(int64_t)(int32_t)(uint32_t)v;
  TakeFlags(s);
  return r;
}

static uint64_t F2I64S(RiscvState* s, float32_t a, int to_signed) {
  if (to_signed) {
    int_fast64_t v = f32_to_i64(a, softfloat_roundingMode, true);
    int inv = (softfloat_exceptionFlags & softfloat_flag_invalid) != 0;
    uint64_t r = inv ? InvalidToI64(a.v, 0, 1) : (uint64_t)(int64_t)v;
    TakeFlags(s);
    return r;
  }
  uint_fast64_t v = f32_to_ui64(a, softfloat_roundingMode, true);
  int inv = (softfloat_exceptionFlags & softfloat_flag_invalid) != 0;
  uint64_t r = inv ? InvalidToI64(a.v, 0, 0) : (uint64_t)v;
  TakeFlags(s);
  return r;
}

static uint64_t F2I32D(RiscvState* s, float64_t a, int to_signed) {
  if (to_signed) {
    int_fast32_t v = f64_to_i32(a, softfloat_roundingMode, true);
    int inv = (softfloat_exceptionFlags & softfloat_flag_invalid) != 0;
    uint64_t r = inv ? InvalidToI32(a.v, 1, 1) : (uint64_t)(int64_t)(int32_t)v;
    TakeFlags(s);
    return r;
  }
  uint_fast32_t v = f64_to_ui32(a, softfloat_roundingMode, true);
  int inv = (softfloat_exceptionFlags & softfloat_flag_invalid) != 0;
  uint64_t r = inv ? InvalidToI32(a.v, 1, 0) : (uint64_t)(int64_t)(int32_t)(uint32_t)v;
  TakeFlags(s);
  return r;
}

static uint64_t F2I64D(RiscvState* s, float64_t a, int to_signed) {
  if (to_signed) {
    int_fast64_t v = f64_to_i64(a, softfloat_roundingMode, true);
    int inv = (softfloat_exceptionFlags & softfloat_flag_invalid) != 0;
    uint64_t r = inv ? InvalidToI64(a.v, 1, 1) : (uint64_t)(int64_t)v;
    TakeFlags(s);
    return r;
  }
  uint_fast64_t v = f64_to_ui64(a, softfloat_roundingMode, true);
  int inv = (softfloat_exceptionFlags & softfloat_flag_invalid) != 0;
  uint64_t r = inv ? InvalidToI64(a.v, 1, 0) : (uint64_t)v;
  TakeFlags(s);
  return r;
}

uint64_t RiscvFpF2IS(RiscvState* s, uint64_t ra, int to_signed, int dbits, int rm) {
  BeginOp(rm);
  float32_t a = F32(SrcS(ra));
  return dbits == 32 ? F2I32S(s, a, to_signed) : F2I64S(s, a, to_signed);
}

uint64_t RiscvFpF2ID(RiscvState* s, uint64_t ra, int to_signed, int dbits, int rm) {
  BeginOp(rm);
  float64_t a = F64(ra);
  return dbits == 32 ? F2I32D(s, a, to_signed) : F2I64D(s, a, to_signed);
}

uint64_t RiscvFpI2FS(RiscvState* s, uint64_t val, int val_signed, int bits, int rm) {
  BeginOp(rm);
  float32_t r;
  if (val_signed)
    r = bits == 32 ? i32_to_f32((int32_t)val) : i64_to_f32((int64_t)val);
  else
    r = bits == 32 ? ui32_to_f32((uint32_t)val) : ui64_to_f32(val);
  TakeFlags(s);
  return Box32(r.v);
}

uint64_t RiscvFpI2FD(RiscvState* s, uint64_t val, int val_signed, int bits, int rm) {
  BeginOp(rm);
  float64_t r;
  if (val_signed)
    r = bits == 32 ? i32_to_f64((int32_t)val) : i64_to_f64((int64_t)val);
  else
    r = bits == 32 ? ui32_to_f64((uint32_t)val) : ui64_to_f64(val);
  TakeFlags(s);
  return r.v;
}

uint64_t RiscvFpCvtSD(RiscvState* s, uint64_t ra, int rm) {
  // the input is a double: NaN boxing does not apply to D values
  BeginOp(rm);
  float32_t r = f64_to_f32(F64(ra));
  TakeFlags(s);
  return Box32(Canon32(r.v));
}

uint64_t RiscvFpCvtDS(RiscvState* s, uint64_t ra, int rm) {
  BeginOp(rm);
  float64_t r = f32_to_f64(F32(SrcS(ra)));
  TakeFlags(s);
  return Canon64(r.v);
}
