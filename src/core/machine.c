#include <stdlib.h>
#include "core/machine.h"
#include "util/log.h"

void MachineRun(Machine *m, uint64_t max_inst) {
  while (!m->cpu.halted) {
    m->isa->Step(&m->cpu);
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
  free(m);
}
