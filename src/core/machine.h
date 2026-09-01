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
  // Reset state, applied after the image loads (QEMU-style boot flow):
  // 0 = enter at the loaded image entry.
  uint64_t reset_pc;
  // Whether raw binaries of this machine speak HTIF (spike does; the QEMU
  // virt and PC contracts do not). ELFs always declare HTIF by symbols.
  int bin_htif;
  // Time-driven device refresh (CLINT MTIP, PIT counters); called by the
  // run loop every step and more often while the CPU sleeps.
  void (*poll)(struct Machine *m);
  // Machine-specific teardown of non-RAM devices; MachineDestroy calls it
  // after freeing the standard parts.
  void (*destroy)(struct Machine *m);
} Machine;

void MachineRun(Machine *m, uint64_t max_inst);
void MachineDestroy(Machine *m);

#endif
