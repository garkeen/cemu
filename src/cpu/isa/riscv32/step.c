// The riscv32 step protocol: interrupt delivery, fetch, one commit.
//
// A step is the small-step operational semantics' single transition: given the
// current CPU state it produces the next one, and rv32_cpu->pc is the one commit
// point. A fault unwinds the whole step through the raise landing pad, so no
// partial state ever survives a faulting instruction.
//
// The instruction set itself (the giant switch) is exec.c.

#include "cpu/isa/isa.h"
#include "debug/debug.h"
#include "exec.h"  // last: it defines the short register macros x/f/rs

// gdb signal for a delivered trap (stage 3.5): breakpoints and environment
// calls read as SIGTRAP, illegal as SIGILL, every access fault as SIGSEGV.
static int GdbTrapSignal(uint64_t cause) {
  switch (cause) {
    case kExIllegal:
      return 4;  // SIGILL
    case kExBreakpoint:
    case kExUserEcall:
    case kExSupervisorEcall:
    case kExMachineEcall:
      return 5;  // SIGTRAP
    default:
      return 11;  // SIGSEGV: fetch/load/store faults and misaligned accesses
  }
}

// Interrupt delivery and the Sstc timer line are sampled at the board's
// 1024-step service boundaries, not every instruction (the riscv64 backend
// and TinyEMU's riscv_cpu_interp do the same). Any instruction boundary is an
// architecturally valid delivery point, so this only bounds latency; aligning
// with the board means a line raised by the device poll is taken in the same
// batch. A sleeping CPU samples every step so wfi still wakes on the first
// pending line.
enum { kStepBatch = 1 << 10 };

// Two streams from one body: the normal run's function has no debug branch
// (STEP_DBG=0 drops every block), the observation stream keeps them.
#define STEP_DBG 0
#define STEP_FN riscv32_step
#include "step_body.inc"
#undef STEP_DBG
#undef STEP_FN

#define STEP_DBG 1
#define STEP_FN riscv32_step_debug
#include "step_body.inc"
#undef STEP_DBG
#undef STEP_FN
