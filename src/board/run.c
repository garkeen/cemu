#include <stdlib.h>

#include "board/board.h"
#include "debug/debug.h"
#include "host/host.h"
#include "util/log.h"

void BoardRun(Board* m, uint64_t max_inst) {
  DebugInit();
  while (!m->cpu.halted) {
    if (m->poll) m->poll(m);
    int asleep = m->cpu.wait;
    if (asleep) {
      // Asleep (wfi/hlt): yield the host; Step still runs so the ISA can
      // observe the wakeup and retire the instruction it resumes with.
      HostSleepMs(1);
    }
    m->isa->step(&m->cpu);
    if (asleep && m->cpu.wait) continue;  // no instruction retired
    m->cpu.inst_count++;
    if (max_inst && m->cpu.inst_count >= max_inst) {
      LogError("instruction limit %llu reached at pc=%llx", (unsigned long long)max_inst,
               (unsigned long long)m->cpu.pc);
      break;
    }
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
