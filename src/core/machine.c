#include <stdlib.h>
#include "core/machine.h"
#include "util/log.h"

Machine *MachineCreateSpike(uint64_t ram_size, uint64_t ram_base) {
  Machine *m = (Machine *)calloc(1, sizeof(Machine));
  if (!m) return NULL;
  m->name = "spike";
  m->ram = RamCreate(ram_base, ram_size);
  if (!m->ram) {
    free(m);
    return NULL;
  }
  BusAddRamRegion(&m->bus, ram_base, ram_size, &kRamOps, m->ram, m->ram->mem);
  m->cpu.halted = kCpuRunning;
  m->cpu.bus = &m->bus;
  return m;
}

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
