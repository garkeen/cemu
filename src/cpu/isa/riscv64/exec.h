#ifndef CEMU_CPU_ISA_RISCV64_EXEC_H
#define CEMU_CPU_ISA_RISCV64_EXEC_H

// Private to the riscv64 interpreter. exec.c (the instruction set: one giant
// switch, a case per manual page) and step.c (the step protocol: interrupt
// delivery, fetch, one commit) are one conceptual translation unit split for
// readability; this is the little they share.
//
// Nothing outside cpu/isa/riscv64/ includes this.
#include "cpu/isa/riscv64/riscv.h"
#include "cpu/step.h"

// The per-step CPU view. riscv_step installs these before any helper runs;
// the manual-notation macros below read them.
extern CpuState *cpu;
extern RiscvState *rs;

// The register banks, in the manual's own notation. x and f are aliases (no
// token collides with them); the pc is never a macro — one named pc would eat
// the cpu->pc member. x[0] is hardwired zero; the commit clears it.
#define x (cpu->gpr)
#define f (cpu->fpr)

// The switch itself: execute one instruction and return the next pc.
uint64_t riscv_exec_inst(frame *fr, uint32_t inst);    // 4-byte instruction
uint64_t riscv_exec_c(frame *fr, uint16_t inst);       // compressed (2-byte)

#endif
