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

// Consumes the prefix stream then dispatches the opcode. Prefixes flip
// w32/a32, pick a segment override and a rep mode; LOCK is a single-hart
// no-op. The first non-prefix byte is the opcode (d.nxt backs up over it).
static void do_step(void) {
  for (;;) {
    uint8_t b = fetch8();
    if (b == 0x66) d.w32 = !d.w32;
    else if (b == 0x67) d.a32 = !d.a32;
    else if (b == 0x26) d.seg = es_i;
    else if (b == 0x2e) d.seg = cs_i;
    else if (b == 0x36) d.seg = ss_i;
    else if (b == 0x3e) d.seg = ds_i;
    else if (b == 0x64) d.seg = fs_i;
    else if (b == 0x65) d.seg = gs_i;
    else if (b == 0xf0) {}
    else if (b == 0xf2) d.rep = 2;
    else if (b == 0xf3) d.rep = 1;
    else {
      run_op(b);  // b is the opcode; d.nxt already counts it
      return;
    }
  }
}

void x86_step(CpuState *c) {
  cpu = c;
  s = (x86_state *)cpu->priv;
  fl = &s->fl;
  static frame f;
  fr = &f;
  fr->cpu = cpu;
  fr->priv = s;
  fr->isa = &k_isa_x86;

  // Hardware interrupts are sampled between instructions, when IF=1 and
  // outside the SDM inhibit window (the instruction after STI). A hardware
  // interrupt is a trap: the pushed return address is the next instruction.
  if (s->intr_pending && !s->intr_inhibit && fl->if_ && cpu->int_ack) {
    int vec = cpu->int_ack(cpu->ack_dev);
    s->intr_pending = 0;
    cpu->wait = 0;
    fr->rec.pc = cpu->pc;
    fr->rec.dnpc = cpu->pc;
    fr->rec.raw_len = 0;
    fr->rec.mnemonic = "intr";
    do_int(vec, (uint32_t)cpu->pc);
    DebugInsn(fr);
    return;
  }

  memset(&g, 0, sizeof(g));
  d.seg = -1;
  // Default sizes come from the CS D-bit (SDM: operand and address default
  // together in a code segment); prefixes then flip either one.
  d.w32 = (s->cr0 & 1) && s->dbit[cs_i];
  d.a32 = d.w32;
  d.code16 = !((s->cr0 & 1) && s->dbit[cs_i]);  // wrap unless CS is 32-bit
  fr->rec.pc = cpu->pc;
  fr->rec.dnpc = cpu->pc;
  fr->rec.raw_len = 0;
  fr->rec.len = 0;
  fr->rec.mnemonic = NULL;

  if (setjmp(fr->raise)) {
    // #DE/#UD are faults: re-deliver through the IVT, resume at the handler.
    int vec = fr->trap.cause == vec_ud ? vec_ud : vec_de;
    do_int(vec, (uint32_t)fr->trap.tval);
    DebugTrap(fr);
    s->intr_inhibit = 0;
    return;
  }

  do_step();
  // The one commit. d.nxt counts consumed bytes from the instruction start;
  // relative jumps rewrote it relatively, absolute cases set eip themselves.
  // 16-bit code wraps the instruction pointer at 64K (SDM rel16 semantics).
  if (eip == fr->rec.pc) cpu->pc = fr->rec.pc + d.nxt;
  if (d.code16) cpu->pc &= 0xffff;
  fr->rec.dnpc = cpu->pc;
  DebugInsn(fr);
  s->intr_inhibit = 0;
}
