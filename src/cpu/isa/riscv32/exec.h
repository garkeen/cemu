#ifndef CEMU_CPU_ISA_RISCV32_EXEC_H
#define CEMU_CPU_ISA_RISCV32_EXEC_H

// Private to the riscv32 interpreter. exec.c (the instruction set: one giant
// switch, a case per manual page) and step.c (the step protocol: interrupt
// delivery, fetch, one commit) are one conceptual translation unit split for
// readability; this is the little they share.
//
// Nothing outside cpu/isa/riscv32/ includes this.
#include "cpu/isa/riscv32/riscv32.h"
#include "cpu/step.h"

// The per-step CPU view. riscv32_step installs these before any helper runs;
// the manual-notation macros below read them. The pointers carry rv32_ names
// so this interpreter can be linked beside riscv64 and x86 in one binary.
extern CpuState* rv32_cpu;
extern Riscv32State* rv32_rs;

// The 32-bit register bank: the interpreter runs on a local copy and step.c
// commits it back, so C arithmetic wraps at 32 bits and a faulting
// instruction's writes roll back as a whole.
extern uint32_t rv32_x[32];

// The manual's own notation. x is the 32-bit bank; f is the shared FP bank
// (NaN-boxed, FLEN=64 with D). Neither token collides with a parameter name
// in this directory. x[0] is hardwired zero; the bank reload and the commit
// clear it.
#define rs rv32_rs
#define x rv32_x
#define f (rv32_cpu->fpr)

// The switch itself: execute one instruction and return the next pc.
uint64_t riscv32_exec_inst(frame* fr, uint32_t inst);  // 4-byte instruction
uint64_t riscv32_exec_c(frame* fr, uint16_t inst);     // compressed (2-byte)

#endif
