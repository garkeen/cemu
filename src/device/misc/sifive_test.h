#ifndef CEMU_DEVICE_MISC_SIFIVE_TEST_H
#define CEMU_DEVICE_MISC_SIFIVE_TEST_H

#include <stdint.h>
#include "bus/bus.h"
#include "cpu/cpu.h"

// SiFive test finisher at 0x100000 on the virt machine.
typedef struct SifiveTestDevice {
  CpuState *cpu;
} SifiveTestDevice;

extern const DeviceOps kSifiveTestOps;

void SifiveTestBind(SifiveTestDevice *dev, CpuState *cpu);
void SifiveTestRegister(Bus *bus, SifiveTestDevice *dev, uint64_t base,
                        uint64_t size);

#endif
