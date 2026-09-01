#include <stdlib.h>
#include "core/machine.h"
#include "host/host.h"
#include "util/log.h"

void MachineRun(Machine *m, uint64_t max_inst) {
  while (!m->cpu.halted) {
    if (m->poll) m->poll(m);
    int asleep = m->cpu.wait;
    if (asleep) {
      // Asleep (wfi/hlt): yield the host; Step still runs so the ISA can
      // observe the wakeup and retire the instruction it resumes with.
      HostSleepMs(1);
    }
    m->isa->Step(&m->cpu);
    if (asleep && m->cpu.wait) continue;  // no instruction retired
    m->cpu.inst_count++;
    if (max_inst && m->cpu.inst_count >= max_inst) {
      LogError("instruction limit %llu reached at pc=%llx",
               (unsigned long long)max_inst, (unsigned long long)m->cpu.pc);
      break;
    }
  }
}

void MachineDestroy(Machine *m) {
  if (!m) return;
  RamDestroy(m->ram);
  if (m->destroy) m->destroy(m);
  free(m);
}
