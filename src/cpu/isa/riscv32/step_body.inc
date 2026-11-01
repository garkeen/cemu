// Step body template, included twice by step.c:
//   STEP_DBG=0 -> the normal run; the preprocessor removes every debug block
//   STEP_DBG=1 -> the observation stream (rec/frame/hub calls)
// The opcode interpreter stays shared; only this per-step wrapper is
// instantiated twice, so the normal path has no debug branch.

void STEP_FN(CpuState* c) {
  rv32_cpu = c;
  rs = (Riscv32State*)rv32_cpu->priv;
  static frame fbuf;
  frame* fr = &fbuf;
  fr->cpu = rv32_cpu;
  fr->priv = rs;
  fr->isa = &k_isa_riscv32;
  // Live frame for the debug hub (debug.h): rows with no frame of their own
  // report this instruction's linear PC. Memory/watch rows carry their own
  // frame, so only the frameless categories need this.
#if STEP_DBG
  if (g_dbg_frame) DebugSetFrame(fr);
#endif

  // Every architectural address is 32 bits: a stale high half (a debugger
  // write, a loader image entry) must not leak into fetch arithmetic.
  rv32_cpu->pc = (uint32_t)rv32_cpu->pc;

  int boundary = rv32_cpu->wait || (rv32_cpu->inst_count & (kStepBatch - 1)) == 0;
  if (boundary) riscv32_sync_timer(rv32_cpu, rs);

  // wfi sleep: no fetch until an enabled interrupt is pending; rv32_cpu->wait is
  // the one sleep flag and the board run loop keeps the timers running.
  if (rv32_cpu->wait) {
    if (((rs->mip | rs->ext_irq) & rs->mie) == 0) return;
    rv32_cpu->wait = 0;
  }
  if (boundary) {
    riscv32_deliver_interrupt(rv32_cpu, rs);
    if (rv32_cpu->halted) return;
  }

  // The interpreter's 32-bit bank is authoritative inside the step; the
  // committed bank is copied back only when the instruction retires.
  Riscv32LoadRegs(rv32_cpu);

  if (setjmp(fr->raise)) {
    // The trap consumer: take it and land on the vector. Register writes made
    // before the fault are dropped with the local bank (instruction
    // atomicity).
    rv32_cpu->pc = (uint32_t)riscv32_trap(rv32_cpu, rs, fr->trap.cause, fr->trap.tval);
    fr->rec.dnpc = rv32_cpu->pc;
    // gdb stub: a trap landed during the step that just ran (step.h hook).
    rs->trap_seq++;
    rs->trap_signal = (uint8_t)GdbTrapSignal(fr->trap.cause);
    DebugTrap(fr);
  } else {
    if (rv32_cpu->pc & 1) raise_(fr, kExFetchMisaligned, rv32_cpu->pc);
    int c_on = (int)((rs->misa >> 2) & 1);
    // With misa.C clear IALIGN is 32: a 32-bit fetch at a 2-aligned address
    // is instruction-address-misaligned (priv spec 2.2).
    if (!c_on && rv32_cpu->pc % 4) raise_(fr, kExFetchMisaligned, rv32_cpu->pc);

    fr->rec.pc = rv32_cpu->pc;
    fr->rec.dnpc = rv32_cpu->pc + 2;  // provisional; corrected to +4 below
#if STEP_DBG
      fr->rec.raw_len = 0;
      fr->rec.mnemonic = NULL;  // per-step reset (the frame buffer is static)
#endif
    uint32_t inst;
    int ilen;
    if (c_on && (rv32_cpu->pc & 3) == 0) {
      // A 4-aligned fetch cannot cross its 4 KiB page, and C code still puts
      // the deciding low halfword first: one 32-bit read fetches both cases.
      inst = (uint32_t)riscv32_mem_load(fr, rv32_cpu->pc, 4, acc_ifetch);
      if ((inst & 3) != 3) {
        inst &= 0xffff;
        ilen = 2;
      } else {
        ilen = 4;
        fr->rec.dnpc = rv32_cpu->pc + 4;
      }
    } else {
      uint16_t half = (uint16_t)riscv32_mem_load(fr, rv32_cpu->pc, 2, acc_ifetch);
      if (c_on && (half & 3) != 3) {
        inst = half;
        ilen = 2;
      } else {
        uint16_t high = (uint16_t)riscv32_mem_load(fr, rv32_cpu->pc + 2, 2, acc_ifetch);
        inst = (uint32_t)half | ((uint32_t)high << 16);
        ilen = 4;
        fr->rec.dnpc = rv32_cpu->pc + 4;
      }
    }
#if STEP_DBG
      fr->rec.raw[0] = (uint8_t)inst;
      fr->rec.raw[1] = (uint8_t)(inst >> 8);
      fr->rec.raw_len = (uint8_t)ilen;
      if (ilen == 4) {
        fr->rec.raw[2] = (uint8_t)(inst >> 16);
        fr->rec.raw[3] = (uint8_t)(inst >> 24);
      }
      fr->rec.len = (uint8_t)ilen;
#endif

    // Execute triggers fire on the fetched pc before the instruction runs
    // (Sdtrig 5.3.12; xiangshanNEMU checks at decode — after the fetch, so a
    // fetch fault keeps precedence).
    Riscv32TriggerCheck(fr, rs, kTrigOpExecute, rv32_cpu->pc);

    // The one commit: the interpreter returns the new pc; x0 stays zero and
    // the 32-bit bank becomes the architectural register file again.
    x[0] = 0;
    rv32_cpu->pc = (uint32_t)(ilen == 2 ? riscv32_exec_c(fr, (uint16_t)inst)
                                   : riscv32_exec_inst(fr, inst));
    Riscv32StoreRegs(rv32_cpu);
    fr->rec.dnpc = rv32_cpu->pc;
#if STEP_DBG
    if (g_dbg_insn) DebugInsn(fr);
#endif
  }

  // Counter increments gated by mcountinhibit; a write to a counter
  // suppresses its own increment for the writing instruction (Zicntr).
  if (!(rs->mcountinhibit & kMcountinhibitIr) && rs->counter_written != kCounterMinstret)
    rs->minstret++;
  if (!(rs->mcountinhibit & kMcountinhibitCy) && rs->counter_written != kCounterMcycle)
    rs->mcycle++;
  rs->counter_written = kCounterNone;
}
