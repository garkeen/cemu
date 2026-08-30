#ifndef CEMU_CORE_MACHINE_H
#define CEMU_CORE_MACHINE_H

#include "core/bus.h"
#include "core/cpu.h"
#include "isa/isa.h"
#include "device/ram.h"
#include "device/htif.h"

// A machine is a device table plus reset state. Machines are ISA-agnostic:
// the ISA rides in IsaOps and is picked at image load time.
typedef struct Machine {
  const char *name;
  Bus bus;
  Bus io;             // port I/O space; used only by machines whose ISA has one
  CpuState cpu;
  const IsaOps *isa;
  HtifDevice htif;    // attached only when the loaded image speaks HTIF
  RamDevice *ram;     // main RAM, freed by MachineDestroy
  uint64_t bin_base;  // where the machine expects raw images to load
} Machine;

void MachineRun(Machine *m, uint64_t max_inst);
void MachineDestroy(Machine *m);

#endif
