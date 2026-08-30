#ifndef CEMU_DEVICE_RAM_H
#define CEMU_DEVICE_RAM_H

#include <stdint.h>
#include "core/bus.h"

typedef struct RamDevice {
  uint8_t *mem;
  uint64_t base;
  uint64_t size;
} RamDevice;

extern const DeviceOps kRamOps;

RamDevice *RamCreate(uint64_t base, uint64_t size);
void RamDestroy(RamDevice *ram);

#endif
