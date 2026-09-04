#ifndef CEMU_BUS_BUS_H
#define CEMU_BUS_BUS_H

#include <stdint.h>

#include "util/type.h"

typedef struct DeviceOps DeviceOps;

struct DeviceOps {
  const char* name;
  uint64_t (*Read)(void* dev, uint64_t addr, int size);
  void (*Write)(void* dev, uint64_t addr, int size, uint64_t val);
};

typedef struct BusRegion {
  uint64_t base;
  uint64_t size;
  const DeviceOps* ops;
  void* dev;
  // Direct host backing store. RAM regions expose theirs so the loader can
  // copy images in without device callbacks (same shape as dearchap-tinyemu
  // iomem.h PhysMemoryRange.is_ram/phys_mem); MMIO devices leave it NULL.
  uint8_t* host;
} BusRegion;

typedef struct Bus {
  BusRegion regions[32];
  int count;
} Bus;

// Registers an MMIO device window (accesses go through ops->Read/Write).
void BusAddRegion(Bus* bus, uint64_t base, uint64_t size, const DeviceOps* ops, void* dev);
// Registers RAM with its host backing store for direct access.
void BusAddRamRegion(Bus* bus, uint64_t base, uint64_t size, const DeviceOps* ops, void* dev,
                     uint8_t* host);
int BusProbe(Bus* bus, uint64_t addr, int len, BusRegion** out);
uint64_t BusRead(Bus* bus, uint64_t addr, int size);
void BusWrite(Bus* bus, uint64_t addr, int size, uint64_t val);
// Returns the host pointer backing [addr, addr+len) inside a RAM region.
int BusRamRange(Bus* bus, uint64_t addr, uint64_t len, uint8_t** host);

#endif
