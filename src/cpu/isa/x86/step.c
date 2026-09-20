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

// Consumes the prefix stream then dispatches the opcode. Prefixes flip
// w32/a32, pick a segment override and a rep mode; LOCK is a single-hart
// no-op. The first non-prefix byte is the opcode (d.nxt backs up over it).
static void do_step(void) {
  for (;;) {
    uint8_t b = fetch8();
    if (b == 0x66)
      d.w32 = !d.w32;
    else if (b == 0x67)
      d.a32 = !d.a32;
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
    } else if (b == 0xf2)
      d.rep = 2;
    else if (b == 0xf3)
      d.rep = 1;
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

void x86_step(CpuState* c) {
  cpu = c;
  s = (x86_state*)cpu->priv;
  fl = &s->fl;
  static frame f;
  fr = &f;
  fr->cpu = cpu;
  fr->priv = s;
  fr->isa = &k_isa_x86;
  // The vector currently being delivered, or -1. A fault inside that window
  // escalates to #DF, and a fault during #DF delivery is a triple fault —
  // shutdown (SDM vol.3 6.9). Without this the landing pad would ping-pong
  // between the two forever.
  static int delivering_vec = -1;

  // The one-instruction INTR inhibit shadow expires here (read once, then
  // cleared below with the sample point after the landing pad).
  int intr_shadow = s->intr_inhibit;
  s->intr_inhibit = 0;

  memset(&g, 0, sizeof(g));
  d.seg = -1;
  // Default sizes come from the CS D-bit (SDM: operand and address default
  // together in a code segment); prefixes then flip either one.
  d.w32 = (s->cr0 & 1) && s->dbit[cs_i];
  d.a32 = d.w32;
  // The 64K instruction-pointer wrap is not latched here: it is decided after
  // the instruction, from the segment it leaves the CPU in (see the commit).
  fr->rec.pc = cpu->pc;
  fr->rec.dnpc = cpu->pc;
  fr->rec.raw_len = 0;
  fr->rec.len = 0;
  fr->rec.mnemonic = NULL;

  if (setjmp(fr->raise)) {
    // Faults re-deliver through the interrupt table at the raised vector and
    // resume at the faulting instruction (SDM 6-3 fault semantics); tval is
    // the error code for vectors that push one. A fault while a vector is
    // being delivered escalates to #DF; #DF failing its own delivery is a
    // triple fault — shutdown (SDM vol.3 6.9).
    int cause = (int)fr->trap.cause;
    uint32_t tval = (uint32_t)fr->trap.tval;
    // The trap category reports the exception at the raise point, not after
    // delivery: a fault whose own delivery faults (the escalation below, up to
    // the triple fault that ends the run) never reaches a post-delivery
    // report, and that report is the whole story of what went wrong.
    DebugTrap(fr);
    int was = delivering_vec;
    delivering_vec = -1;
    if (was >= 0) {
      if (was == vec_df) Fatal("x86: triple fault");
      cause = vec_df;
      tval = 0;  // #DF pushes error code 0 (SDM vol.3 table 6-1)
    }
    delivering_vec = cause;
    do_int(cause, (uint32_t)cpu->pc, kIntException, tval);
    delivering_vec = -1;
    // Live RF clears on exception entry; the pushed image kept whatever the
    // interrupted context held (an instruction-breakpoint #DB forces RF=1 in
    // its image so the resumed instruction reports nothing, SDM vol.3 17.3.1).
    fl->rf = 0;
    // gdb stub: an exception landed during the step that just ran (step.h
    // hook). Hardware INTR delivery above intentionally does not count.
    s->trap_seq++;
    s->trap_signal = (uint8_t)GdbTrapSignal(cause);
    return;
  }

  // Instruction breakpoints (SDM vol.3 17.3.1): a #DB FAULT on the fetched
  // linear address, before the instruction runs — priority above the pending
  // INTR (vol.3 table 6-2). RF suppresses the match for one instruction.
  if (s->dr[7] & 0xff) {
    uint64_t lin = s->base[cs_i] + (uint32_t)cpu->pc;
    for (int i = 0; i < 4; i++) {
      if (!(s->dr[7] & (3u << (2 * i)))) continue;          // Li/Gi enable
      if (((s->dr[7] >> (16 + 4 * i)) & 3) != 0) continue;  // R/W=00: execute
      if (lin == (uint64_t)s->dr[i] && !fl->rf) {
        s->dr[6] |= 1u << i;
        fl->rf = 1;  // the saved image resumes past this instruction
        raise_(fr, vec_db, 0);
      }
    }
  }

  // Hardware interrupts are sampled between instructions (after the landing
  // pad, so a fault during delivery escalates inside this step), when IF=1
  // and outside the SDM inhibit window (the instruction after STI/MOV SS/
  // POP SS — the shadow was read and cleared at step entry). A hardware
  // interrupt is a trap: the pushed return address is the next instruction.
  if (s->intr_pending && !intr_shadow && fl->if_ && cpu->int_ack) {
    int vec = cpu->int_ack(cpu->ack_dev);
    s->intr_pending = 0;
    cpu->wait = 0;
    fr->rec.pc = cpu->pc;
    fr->rec.dnpc = cpu->pc;
    fr->rec.raw_len = 0;
    fr->rec.mnemonic = "intr";
    delivering_vec = vec;
    do_int(vec, (uint32_t)cpu->pc, kIntExternal, 0);
    delivering_vec = -1;
    DebugInsn(fr);
    return;
  }

  // HLT semantics (SDM vol.2 HLT; vol.3 17-17 "halt state"): instruction
  // execution stops — nothing after the hlt runs until the wake above
  // delivers. The step observes the wakeup and retires nothing;
  // BoardRunSteps keeps stepping (and polling its stop callback) meanwhile.
  if (cpu->wait) return;

  // Single-step is decided at the instruction boundary: the TF value before
  // the instruction ran, not after (POPF/IRET setting TF trap one
  // instruction later, SDM vol.3 17.3.1).
  int tf0 = fl->tf;
  do_step();
  // The one commit. d.nxt counts consumed bytes from the instruction start;
  // relative jumps rewrote it relatively, absolute cases set eip themselves.
  // EIP is 32 bits — the sum wraps at 2^32 (a negative rel32 rides on the
  // wrap); 16-bit code then wraps the instruction pointer at 64K.
  if (eip == fr->rec.pc) cpu->pc = (uint32_t)(fr->rec.pc + d.nxt);
  // In a 16-bit code segment the instruction pointer wraps at 64K (SDM vol.1
  // 3.7.1). The test reads the segment the CPU is in *after* the instruction,
  // not the one it entered with: a far jump that lands in a 32-bit segment
  // keeps all 32 bits of its target (SeaBIOS's transition32 makes exactly that
  // jump — truncating on the old segment's D bit turns 0xfc9e4 into 0xc9e4 and
  // the CPU walks off into whatever happens to sit at the bottom of RAM).
  if (!((s->cr0 & kCr0Pe) && s->dbit[cs_i])) cpu->pc &= 0xffff;
  fr->rec.dnpc = cpu->pc;
  // RF clears after the instruction it protected completed successfully —
  // unless this instruction itself loaded an EFLAGS image (iret/popf/task
  // switch), whose RF is the authoritative one (SDM vol.3 17.3.1).
  if (!d.rf_load) fl->rf = 0;
  // Debug traps (SDM vol.3 17.3.1, priority table 6-2): the task-switch trap
  // (TSS.T), data/I-O watchpoints and single-step all fire after the
  // instruction completes, reporting the next instruction's address in one
  // #DB. A step that delivered an exception frame (int3/icebp/INT n) takes
  // no debug trap here — its handler runs unstepped (do_int clears live TF;
  // the saved image keeps it so IRET resumes stepping).
  uint32_t dr6set = d.watch_hit;
  if (s->bt_pending) dr6set |= kDr6Bt;
  if (tf0) dr6set |= kDr6Bs;
  if (s->bt_pending || (!d.delivered && (d.watch_hit || tf0))) {
    s->dr[6] |= dr6set;
    s->bt_pending = 0;
    cpu->wait = 0;  // a #DB after hlt wakes the processor
    DebugInsn(fr);
    do_int(vec_db, (uint32_t)cpu->pc, kIntException, 0);
    s->trap_seq++;
    s->trap_signal = (uint8_t)GdbTrapSignal(vec_db);
    DebugTrap(fr);
    return;
  }
  DebugInsn(fr);
}
