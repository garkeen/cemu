#ifndef CEMU_DEVICE_MISC_HTIF_H
#define CEMU_DEVICE_MISC_HTIF_H

#include <stdint.h>
#include "bus/bus.h"
#include "cpu/cpu.h"

// Host-target interface, same contract spike uses: the guest writes tohost,
// the device acts and clears it. Exit encoding: payload = (code << 1) | 1.
typedef struct HtifDevice {
  uint64_t tohost_addr;
  uint64_t fromhost_addr;
  uint64_t tohost;
  uint64_t fromhost;
  CpuState *cpu;
} HtifDevice;

extern const DeviceOps kHtifOps;

void HtifBind(HtifDevice *htif, CpuState *cpu);
void HtifRegister(Bus *bus, HtifDevice *htif, uint64_t tohost_addr,
                  uint64_t fromhost_addr);

#endif
