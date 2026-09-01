#include <stdlib.h>
#include "machine/machine.h"

// QEMU -M spike layout: all DRAM at 0x80000000, no devices except HTIF.
static const uint64_t kDramBase = 0x80000000ULL;
static const uint64_t kDramSizeDefault = 256ULL << 20;

Machine *SpikeMachineCreate(const MachineOpts *opts) {
  uint64_t ram_base = opts->ram_base ? opts->ram_base : kDramBase;
  uint64_t ram_size = opts->ram_size ? opts->ram_size : kDramSizeDefault;
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
  m->bin_base = ram_base;  // images load at the DRAM base
  m->bin_htif = 1;  // raw bins follow the riscv-tests HTIF convention
  return m;
}
