#ifndef CEMU_CPU_ISA_X86_SOFTFLOAT_SOFTFLOAT_H
#define CEMU_CPU_ISA_X86_SOFTFLOAT_SOFTFLOAT_H

// The slice of Berkeley SoftFloat 3e the x86 FPU/SSE engine calls. The
// implementation is the unmodified 8086-SSE amalgamation (softfloat.c in this
// directory, from v86's lib/softfloat, itself the upstream release); these
// declarations must match its definitions exactly — the types are the
// library's public ABI, not ours to reshape.
//
// Reference: Hauser, "Berkeley SoftFloat Release 3e" (BSD-3-Clause, the
// license text is at the top of softfloat.c); QEMU target/i386/tcg/
// fpu_helper.c uses the same package for x87 and SSE arithmetic.
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t v;
} float32_t;
typedef struct {
  uint64_t v;
} float64_t;
// Little-endian layout: the significand's low half first, then sign/exponent.
// This is also the in-memory 10-byte x87 extended format (SDM vol.1 4.2.2).
struct extFloat80M {
  uint64_t signif;
  uint16_t signExp;
};
typedef struct extFloat80M extFloat80_t;

enum {
  softfloat_round_near_even = 0,
  softfloat_round_minMag = 1,
  softfloat_round_min = 2,
  softfloat_round_max = 3,
  softfloat_round_near_maxMag = 4,
};
enum {
  softfloat_flag_inexact = 1,
  softfloat_flag_underflow = 2,
  softfloat_flag_overflow = 4,
  softfloat_flag_infinite = 8,
  softfloat_flag_invalid = 16,
};

extern uint_fast8_t softfloat_roundingMode;
extern uint_fast8_t extF80_roundingPrecision;
extern uint_fast8_t softfloat_exceptionFlags;

// extended (x87) arithmetic
void extF80M_add(const extFloat80_t*, const extFloat80_t*, extFloat80_t*);
void extF80M_sub(const extFloat80_t*, const extFloat80_t*, extFloat80_t*);
void extF80M_mul(const extFloat80_t*, const extFloat80_t*, extFloat80_t*);
void extF80M_div(const extFloat80_t*, const extFloat80_t*, extFloat80_t*);
void extF80M_rem(const extFloat80_t*, const extFloat80_t*, extFloat80_t*);
void extF80M_sqrt(const extFloat80_t*, extFloat80_t*);
void extF80M_roundToInt(const extFloat80_t*, uint_fast8_t, bool, extFloat80_t*);
int_fast32_t extF80M_to_i32(const extFloat80_t*, uint_fast8_t, bool);
int_fast64_t extF80M_to_i64(const extFloat80_t*, uint_fast8_t, bool);
int_fast32_t extF80M_to_i32_r_minMag(const extFloat80_t*, bool);
int_fast64_t extF80M_to_i64_r_minMag(const extFloat80_t*, bool);
void i32_to_extF80M(int32_t, extFloat80_t*);
void i64_to_extF80M(int64_t, extFloat80_t*);
void ui32_to_extF80M(uint32_t, extFloat80_t*);
void ui64_to_extF80M(uint64_t, extFloat80_t*);
void f32_to_extF80M(float32_t, extFloat80_t*);
void f64_to_extF80M(float64_t, extFloat80_t*);
float32_t extF80M_to_f32(const extFloat80_t*);
float64_t extF80M_to_f64(const extFloat80_t*);

// single/double (SSE) arithmetic
float32_t f32_add(float32_t, float32_t);
float32_t f32_sub(float32_t, float32_t);
float32_t f32_mul(float32_t, float32_t);
float32_t f32_div(float32_t, float32_t);
float32_t f32_sqrt(float32_t);
float32_t f32_roundToInt(float32_t, uint_fast8_t, bool);
int_fast32_t f32_to_i32(float32_t, uint_fast8_t, bool);
int_fast64_t f32_to_i64(float32_t, uint_fast8_t, bool);
int_fast32_t f32_to_i32_r_minMag(float32_t, bool);
int_fast64_t f32_to_i64_r_minMag(float32_t, bool);
float64_t f32_to_f64(float32_t);
float32_t i32_to_f32(int32_t);
float64_t i32_to_f64(int32_t);
float32_t i64_to_f32(int64_t);
float64_t i64_to_f64(int64_t);
bool f32_eq(float32_t, float32_t);
bool f32_le(float32_t, float32_t);
bool f32_lt(float32_t, float32_t);
bool f32_le_quiet(float32_t, float32_t);
bool f32_lt_quiet(float32_t, float32_t);
bool f32_isSignalingNaN(float32_t);

float64_t f64_add(float64_t, float64_t);
float64_t f64_sub(float64_t, float64_t);
float64_t f64_mul(float64_t, float64_t);
float64_t f64_div(float64_t, float64_t);
float64_t f64_sqrt(float64_t);
float64_t f64_roundToInt(float64_t, uint_fast8_t, bool);
int_fast32_t f64_to_i32(float64_t, uint_fast8_t, bool);
int_fast64_t f64_to_i64(float64_t, uint_fast8_t, bool);
int_fast32_t f64_to_i32_r_minMag(float64_t, bool);
int_fast64_t f64_to_i64_r_minMag(float64_t, bool);
float32_t f64_to_f32(float64_t);
bool f64_eq(float64_t, float64_t);
bool f64_le(float64_t, float64_t);
bool f64_lt(float64_t, float64_t);
bool f64_le_quiet(float64_t, float64_t);
bool f64_lt_quiet(float64_t, float64_t);
bool f64_isSignalingNaN(float64_t);

#endif
