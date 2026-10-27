#ifndef CEMU_CPU_ISA_X86_FPU_H
#define CEMU_CPU_ISA_X86_FPU_H

#include <stdint.h>

#include "cpu/isa/x86/softfloat/softfloat.h"

// The x87 FPU and SSE state (SDM vol.1 ch.8, 10, 11). The register stack is
// kept in physical order; `top` lives in the status word's TOP field, exactly
// as FXSAVE lays it out. Tags are the 2-bit codes of the tag word (00 valid,
// 01 zero, 10 special, 11 empty), maintained on every write.
enum {
  kFpuIe = 1u << 0,   // invalid operation
  kFpuDe = 1u << 1,   // denormal operand
  kFpuZe = 1u << 2,   // divide-by-zero
  kFpuOe = 1u << 3,   // overflow
  kFpuUe = 1u << 4,   // underflow
  kFpuPe = 1u << 5,   // precision
  kFpuSf = 1u << 6,   // stack fault
  kFpuEs = 1u << 7,   // exception summary
  kFpuC0 = 1u << 8,
  kFpuC1 = 1u << 9,
  kFpuC2 = 1u << 10,
  kFpuC3 = 1u << 14,
  kFpuTopShift = 11,
  kFpuTopMask = 7u << kFpuTopShift,
};
enum { kFpuTagValid = 0, kFpuTagZero = 1, kFpuTagSpecial = 2, kFpuTagEmpty = 3 };
// x87 arithmetic operation codes, the D8/DC group's reg field order.
enum { kFAdd = 0, kFMul, kFCom, kFComp, kFSub, kFSubR, kFDiv, kFDivR };
// The floating-point constants FLD1..FLDZ (D9 E8..EE), in that order.
enum { kFld1 = 0, kFldL2T, kFldL2E, kFldPi, kFldLg2, kFldLn2, kFldZ };
// fcomi/ucomi condition codes (SDM FCOMI): C3=equal, C0=less, C2=unordered.
enum { kFCmpE = 1u << 14, kFCmpL = 1u << 8, kFCmpU = kFCmpE | (1u << 10) | kFCmpL };

typedef struct x86_fpu {
  extFloat80_t st[8];
  uint16_t cw;   // control word
  uint16_t sw;   // status word (TOP included)
  uint16_t tw;   // tag word, 2 bits per physical register
  uint32_t fip;  // instruction pointer (offset in FCS)
  uint16_t fcs;
  uint16_t fop;  // low 11 bits are the x87 opcode
  uint32_t fdp;  // data pointer (offset in FDS)
  uint16_t fds;
  // SSE state. mxcsr_mask advertises the writable bits (QEMU's 0xffff for
  // SSE2; the reset value 0x1f80 is the architectural default).
  uint32_t mxcsr;
  uint32_t mxcsr_mask;
  _Alignas(16) uint8_t xmm[8][16];
  int pending_mf;  // an unmasked x87 exception stands (#MF before the next x87)
} x86_fpu;

void fpu_init(x86_fpu* f);   // reset state (FNINIT)
void fpu_finit(x86_fpu* f);  // FNINIT
void fpu_begin(x86_fpu* f);  // configure softfloat from CW, clear its flags
void fpu_end(x86_fpu* f);    // fold softfloat flags into SW, update ES/pending

int fpu_top(const x86_fpu* f);
void fpu_set_top(x86_fpu* f, int top);
int fpu_tag(const x86_fpu* f, int physical);
void fpu_mark(x86_fpu* f, int physical);
void fpu_set_tags(x86_fpu* f, uint16_t tw);
void fpu_refresh(x86_fpu* f);  // recompute ES/#MF after CW or SW changes

// Stack access. get/require returns the indefinite QNaN and records a stack
// fault when the register is empty; set marks the physical register's tag.
extFloat80_t fpu_get(x86_fpu* f, int i);
void fpu_set(x86_fpu* f, int i, extFloat80_t v);
void fpu_push(x86_fpu* f, extFloat80_t v);
void fpu_pop(x86_fpu* f);
void fpu_free(x86_fpu* f, int i);
void fpu_exchange(x86_fpu* f, int i);

// Arithmetic. a is always ST(0) and b the source; sub = a-b, subr = b-a,
// div = a/b, divr = b/a (the encoding's operand order is the caller's).
void fpu_arith(x86_fpu* f, int op, int dst, extFloat80_t b);
void fpu_compare(x86_fpu* f, extFloat80_t b, int quiet);
uint32_t fpu_compare_i(x86_fpu* f, extFloat80_t b, int quiet);
void fpu_test(x86_fpu* f);
void fpu_examine(x86_fpu* f);
void fpu_change_sign(x86_fpu* f);
void fpu_abs(x86_fpu* f);
void fpu_load_const(x86_fpu* f, int which);

// Arithmetic helpers for the remaining D9 forms.
void fpu_f2xm1(x86_fpu* f);
void fpu_fyl2x(x86_fpu* f);
void fpu_fptan(x86_fpu* f);
void fpu_fpatan(x86_fpu* f);
void fpu_fxtract(x86_fpu* f);
void fpu_fprem(x86_fpu* f, int ieee);
void fpu_fdecstp(x86_fpu* f);
void fpu_fincstp(x86_fpu* f);
void fpu_fyl2xp1(x86_fpu* f);
void fpu_fsqrt(x86_fpu* f);
void fpu_fsincos(x86_fpu* f);
void fpu_frndint(x86_fpu* f);
void fpu_fscale(x86_fpu* f);
void fpu_fsin(x86_fpu* f);
void fpu_fcos(x86_fpu* f);
void fpu_ftst(x86_fpu* f);

// Conversions. Loads flag a denormal operand (DE); stores round through
// softfloat and set PE/UE/OE; integer stores set IE on out-of-range and write
// the integer indefinite value (SDM FIST/FISTP).
extFloat80_t fpu_load_f32(x86_fpu* f, uint32_t bits);
extFloat80_t fpu_load_f64(x86_fpu* f, uint64_t bits);
extFloat80_t fpu_load_i16(x86_fpu* f, int16_t v);
extFloat80_t fpu_load_i32(x86_fpu* f, int32_t v);
extFloat80_t fpu_load_i64(x86_fpu* f, int64_t v);
extFloat80_t fpu_load_bcd(x86_fpu* f, const uint8_t bcd[10]);
uint32_t fpu_store_f32(x86_fpu* f, extFloat80_t v);
uint64_t fpu_store_f64(x86_fpu* f, extFloat80_t v);
int16_t fpu_store_i16(x86_fpu* f, extFloat80_t v);
int32_t fpu_store_i32(x86_fpu* f, extFloat80_t v);
int64_t fpu_store_i64(x86_fpu* f, extFloat80_t v);
void fpu_store_bcd(x86_fpu* f, extFloat80_t v, uint8_t out[10]);
int fpu_is_denormal(extFloat80_t v);
void fpu_note_denormal(x86_fpu* f, extFloat80_t v);
// FISTTP: truncate regardless of the control word's rounding mode (SSE3).
int16_t fpu_store_i16_trunc(x86_fpu* f, extFloat80_t v);
int32_t fpu_store_i32_trunc(x86_fpu* f, extFloat80_t v);
int64_t fpu_store_i64_trunc(x86_fpu* f, extFloat80_t v);

// Environment and state images (SDM FSTENV/FNSAVE/FRSTOR, FXSAVE/FXRSTOR).
// `wide` selects the 32-bit operand-size layout (28/108 bytes) over the 16-bit
// one (14/94 bytes).
void fpu_store_env(const x86_fpu* f, uint8_t* out, int wide);
void fpu_load_env(x86_fpu* f, const uint8_t* in, int wide);
void fpu_store_state(const x86_fpu* f, uint8_t* out, int wide);
void fpu_load_state(x86_fpu* f, const uint8_t* in, int wide);
void fpu_fxsave(const x86_fpu* f, uint8_t* out);
void fpu_fxrstor(x86_fpu* f, const uint8_t* in);

// SSE arithmetic: configure softfloat from MXCSR (rounding mode), then fold
// its exception flags into MXCSR. fpu_sse_end returns nonzero when an
// unmasked numeric exception stands; the caller reports #XM or #UD depending
// on CR4.OSXMMEXCPT (SDM vol.3 13.5.2).
void fpu_sse_begin(x86_fpu* f);
int fpu_sse_end(x86_fpu* f);
int fpu_sse_daz(const x86_fpu* f);   // MXCSR.DAZ: flush denormal inputs to 0
int fpu_sse_ftz(const x86_fpu* f);   // MXCSR.FTZ: flush denormal results to 0
void fpu_sse_flush(uint8_t* elem, int bytes);  // zero a denormal element

#endif
