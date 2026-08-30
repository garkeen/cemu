#ifndef CEMU_CORE_MACHINE_H
#define CEMU_CORE_MACHINE_H

#include "core/bus.h"
#include "core/cpu.h"
#include "isa/isa.h"
#include "device/ram.h"
#include "device/htif.h"

// Spike machine layout (QEMU -M spike): DRAM at 0x80000000.
static const uint64_t kSpikeDramBase = 0x80000000ULL;
static const uint64_t kSpikeDramSizeDefault = 256ULL << 20;
// cemu convention for raw bins, overridable via --htif: the image loads at
// the DRAM base and the HTIF tohost register sits 0x1000 above it.
static const uint64_t kRawBinTohostOffset = 0x1000ULL;

typedef struct Machine {
  const char *name;
  Bus bus;
  CpuState cpu;
  const IsaOps *isa;
  HtifDevice htif;
  RamDevice *ram;
} Machine;

Machine *MachineCreateSpike(uint64_t ram_size, uint64_t ram_base);
void MachineRun(Machine *m, uint64_t max_inst);
void MachineDestroy(Machine *m);

#endif
