#ifndef CEMU_MACHINE_MACHINE_H
#define CEMU_MACHINE_MACHINE_H

#include "core/machine.h"

// Command-line overridable knobs; 0 means "machine default". A machine may
// reject values that contradict its layout contract.
typedef struct MachineOpts {
  uint64_t ram_base;
  uint64_t ram_size;
} MachineOpts;

// Creates a machine by name ("spike", ...). Returns NULL for unknown names.
Machine *MachineCreate(const char *name, const MachineOpts *opts);

Machine *SpikeMachineCreate(const MachineOpts *opts);
Machine *X86MachineCreate(const MachineOpts *opts);

#endif
