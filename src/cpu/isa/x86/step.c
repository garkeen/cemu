// The x86 step protocol: INTR sampling, prefixes, one commit.
//
// A step is the small-step operational semantics' single transition: given the
// current CPU state it produces the next one, and cpu->pc is the one commit
// point. A fault unwinds the whole step through the raise landing pad and is
// re-delivered through the IVT, so no partial state survives a faulting
// instruction.
//
// The instruction set itself (the opcode switches) is exec.c.

#include <string.h>

#include "cpu/isa/isa.h"
#include "debug/debug.h"
#include "exec.h"  // last: it defines the short register macros x/f/eax/...
#include "util/log.h"

// Consumes the prefix stream then dispatches the opcode. 66/67 select the
// opposite of the code segment's default size; a prefix is idempotent, so a
// repeated one has the same effect as a single occurrence (SDM vol.2 2.1.1).
// The segment and rep prefixes set their mode; LOCK is a single-hart no-op.
// The first non-prefix byte is the opcode (d.nxt already counts it).
static void do_step(void) {
  int dflt = (s->cr0 & 1) && s->dbit[cs_i];
  for (;;) {
    uint8_t b = fetch8();
    if (b == 0x66) {
      d.w32 = !dflt;
      d.p66 = 1;  // also the mandatory SSE2 prefix
    } else if (b == 0x67)
      d.a32 = !dflt;
    else if (b == 0x26)
      d.seg = es_i;
    else if (b == 0x2e)
      d.seg = cs_i;
    else if (b == 0x36)
      d.seg = ss_i;
    else if (b == 0x3e)
      d.seg = ds_i;
    else if (b == 0x64)
      d.seg = fs_i;
    else if (b == 0x65)
      d.seg = gs_i;
    else if (b == 0xf0) {
      d.lock = 1;
    } else if (b == 0xf2) {
      d.rep = 2;
      d.pf2 = 1;
    } else if (b == 0xf3) {
      d.rep = 1;
      d.pf3 = 1;
    }
    else {
      run_op(b);  // b is the opcode; d.nxt already counts it
      return;
    }
  }
}

// gdb signal for a delivered exception (stage 3.5): #DB/#BP and the INT n
// family read as SIGTRAP, #DE as SIGFPE, #UD as SIGILL, the access-fault
// family as SIGSEGV.
static int GdbTrapSignal(int vec) {
  switch (vec) {
    case 0:
      return 8;  // SIGFPE (#DE)
    case vec_ud:
      return 4;  // SIGILL (#UD)
    case vec_ts:
    case vec_np:
    case vec_ss:
    case vec_gp:
    case vec_pf:
    case vec_ac:
      return 11;  // SIGSEGV
    default:
      return 5;  // SIGTRAP: #DB, #BP, #OF, #BR, ...
  }
}

// Two streams from one body: the normal run's function has no debug branch
// (STEP_DBG=0 drops every block), the observation stream keeps them.
#define STEP_DBG 0
#define STEP_FN x86_step
#include "step_body.inc"
#undef STEP_DBG
#undef STEP_FN

#define STEP_DBG 1
#define STEP_FN x86_step_debug
#include "step_body.inc"
#undef STEP_DBG
#undef STEP_FN
