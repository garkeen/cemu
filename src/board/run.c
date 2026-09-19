#include <stdlib.h>

#include "board/board.h"
#include "debug/debug.h"
#include "debug/gdbstub.h"
#include "host/host.h"
#include "util/log.h"

int BoardRunSteps(Board* m, uint64_t max_inst, int (*stop_cb)(void* ctx, CpuState* cpu),
                  void* cb_ctx) {
  while (!m->cpu.halted) {
    if (m->poll) m->poll(m);
  // Host keys (stdin: the console or a pipe) go to the board's key sink; the
  // poll self-gates, so calling it every loop costs almost nothing.
  HostKeyPoll();
    if (m->display) {
      // Attached display window (-display): pump its message queue and stop
      // the emulation when the user closes it, like QEMU quitting.
      HostDisplayPump(m->display);
      if (HostDisplayClosed(m->display)) {
        LogInfo("display window closed");
        m->cpu.halted = kCpuExited;
        break;
      }
    }
    int asleep = m->cpu.wait;
    if (asleep) {
      // Asleep (wfi/hlt): yield the host; Step still runs so the ISA can
      // observe the wakeup and retire the instruction it resumes with.
      HostSleepMs(1);
    }
    m->isa->step(&m->cpu);
    if (asleep && m->cpu.wait) {
      // Still asleep (no interrupt yet): no instruction retired. The per-step
      // observers still get their look — the gdb stub polls for connecting
      // clients here, the same way the board poll (PitPoll) keeps running
      // "more often while the CPU sleeps" (i8254.h).
      if (stop_cb && stop_cb(cb_ctx, &m->cpu)) return 1;
      continue;
    }
    m->cpu.inst_count++;
    if (stop_cb && stop_cb(cb_ctx, &m->cpu)) return 1;
    if (max_inst && m->cpu.inst_count >= max_inst) {
      LogError("instruction limit %llu reached at pc=%llx", (unsigned long long)max_inst,
               (unsigned long long)m->cpu.pc);
      return 0;
    }
  }
  return 0;
}

void BoardRun(Board* m, uint64_t max_inst) {
  DebugInit();
  if (m->gdb) {
    // The stub owns run control: sessions (attach/step/continue) alternate
    // with free runs until the emulation ends.
    GdbStubRun(m->gdb, max_inst);
  } else {
    BoardRunSteps(m, max_inst, NULL, NULL);
  }
  frame end;
  end.cpu = &m->cpu;
  end.priv = m->cpu.priv;
  end.isa = m->isa;
  DebugSessionEnd(&end, "halt");
}

void BoardDestroy(Board* m) {
  if (!m) return;
  RamDestroy(m->ram);
  if (m->destroy) m->destroy(m);
  free(m);
}
