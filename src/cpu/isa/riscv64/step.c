// The riscv64 step protocol: interrupt delivery, fetch, one commit.
//
// A step is the small-step operational semantics' single transition: given the
// current CPU state it produces the next one, and cpu->pc is the one commit
// point. A fault unwinds the whole step through the raise landing pad, so no
// partial state ever survives a faulting instruction.
//
// The instruction set itself (the giant switch) is exec.c.

#include "cpu/isa/isa.h"
#include "debug/debug.h"
#include "exec.h"  // last: it defines the short register macros x/f/eax/...

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
// 1024-step service boundaries, not every instruction (TinyEMU
// riscv_cpu_interp checks between blocks). Any instruction boundary is an
// architecturally valid delivery point, so this only bounds latency; aligning
// with the board means a line raised by the device poll is taken in the same
// batch. A sleeping CPU samples every step so wfi still wakes on the first
// pending line.
enum { kStepBatch = 1 << 10 };

void riscv_step(CpuState* c) {
  cpu = c;
  rs = (RiscvState*)cpu->priv;
  static frame fbuf;
  frame* fr = &fbuf;
  fr->cpu = cpu;
  fr->priv = rs;
  fr->isa = &k_isa_riscv64;
  // Watchpoints are not a mask bit: DebugOn(kDbgMem) also reports them, and
  // their rows carry the instruction bytes, so keep the record filled then.
  int dbg = DebugOn(kDbgAny) || DebugOn(kDbgMem);

  // Sstc STIP is a timer line, not an instruction-stream fact: sampling the
  // host clock on every step was pure cost. Refresh it at the same batch
  // boundaries (the board polls while the CPU sleeps, so a sleeping guest
  // still wakes on time) — well under the timer's own period.
  int boundary = cpu->wait || (cpu->inst_count & (kStepBatch - 1)) == 0;
  if (boundary) riscv_sync_timer(cpu, rs);

  // wfi sleep: no fetch until an enabled interrupt is pending; cpu->wait is
  // the one sleep flag and the board run loop keeps the timers running.
  if (cpu->wait) {
    if (((rs->mip | rs->ext_irq) & rs->mie) == 0) return;
    cpu->wait = 0;
  }
  if (boundary) {
    riscv_deliver_interrupt(cpu, rs);
    if (cpu->halted) return;
  }

  if (setjmp(fr->raise)) {
    // The trap consumer: take it and land on the vector.
    cpu->pc = riscv_trap(cpu, rs, fr->trap.cause, fr->trap.tval);
    fr->rec.dnpc = cpu->pc;
    // gdb stub: a trap landed during the step that just ran (step.h hook).
    rs->trap_seq++;
    rs->trap_signal = (uint8_t)GdbTrapSignal(fr->trap.cause);
    DebugTrap(fr);
  } else {
    if (cpu->pc & 1) raise_(fr, kExFetchMisaligned, cpu->pc);
    int c_on = (int)((rs->misa >> 2) & 1);
    // With misa.C clear IALIGN is 32: a 32-bit fetch at a 2-aligned address
    // is instruction-address-misaligned (priv spec 2.2).
    if (!c_on && cpu->pc % 4) raise_(fr, kExFetchMisaligned, cpu->pc);

    fr->rec.pc = cpu->pc;
    fr->rec.dnpc = cpu->pc + 2;  // provisional; corrected to +4 below
    if (dbg) {
      fr->rec.raw_len = 0;
      fr->rec.mnemonic = NULL;  // per-step reset (the frame buffer is static)
    }
    uint32_t inst;
    int ilen;
    if (c_on && (cpu->pc & 3) == 0) {
      // A 4-aligned fetch cannot cross its 4 KiB page, and C code still puts
      // the deciding low halfword first: one 32-bit read fetches both cases.
      inst = (uint32_t)riscv_mem_load(fr, cpu->pc, 4, acc_ifetch);
      if ((inst & 3) != 3) {
        inst &= 0xffff;
        ilen = 2;
      } else {
        ilen = 4;
        fr->rec.dnpc = cpu->pc + 4;
      }
    } else {
      uint16_t half = (uint16_t)riscv_mem_load(fr, cpu->pc, 2, acc_ifetch);
      if (c_on && (half & 3) != 3) {
        inst = half;
        ilen = 2;
      } else {
        uint16_t high = (uint16_t)riscv_mem_load(fr, cpu->pc + 2, 2, acc_ifetch);
        inst = (uint32_t)half | ((uint32_t)high << 16);
        ilen = 4;
        fr->rec.dnpc = cpu->pc + 4;
      }
    }
    if (dbg) {
      fr->rec.raw[0] = (uint8_t)inst;
      fr->rec.raw[1] = (uint8_t)(inst >> 8);
      fr->rec.raw_len = (uint8_t)ilen;
      if (ilen == 4) {
        fr->rec.raw[2] = (uint8_t)(inst >> 16);
        fr->rec.raw[3] = (uint8_t)(inst >> 24);
      }
      fr->rec.len = (uint8_t)ilen;
    }

    // Execute triggers fire on the fetched pc before the instruction runs
    // (Sdtrig 5.3.12; xiangshanNEMU checks at decode — after the fetch, so a
    // fetch fault keeps precedence).
    RiscvTriggerCheck(fr, rs, kTrigOpExecute, cpu->pc);

    // The one commit: the interpreter returns the new pc; x0 stays zero.
    x[0] = 0;
    cpu->pc = ilen == 2 ? riscv_exec_c(fr, (uint16_t)inst) : riscv_exec_inst(fr, inst);
    fr->rec.dnpc = cpu->pc;
    if (dbg) DebugInsn(fr);
  }

  // Counter increments gated by mcountinhibit; a write to a counter
  // suppresses its own increment for the writing instruction (Zicntr).
  if (!(rs->mcountinhibit & kMcountinhibitIr) && rs->counter_written != kCounterMinstret)
    rs->minstret++;
  if (!(rs->mcountinhibit & kMcountinhibitCy) && rs->counter_written != kCounterMcycle)
    rs->mcycle++;
  rs->counter_written = kCounterNone;
}
