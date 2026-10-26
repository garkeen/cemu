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
  // Set when another region intersects this one: the smallest-match rule can
  // then pick the other for some address here, so a one-entry lookup cache
  // must not short-circuit this region.
  int overlaps;
  // Direct host backing store. RAM regions expose theirs so the loader can
  // copy images in without device callbacks (same shape as dearchap-tinyemu
  // iomem.h PhysMemoryRange.is_ram/phys_mem); MMIO devices leave it NULL.
  uint8_t* host;
} BusRegion;

typedef struct Bus {
  BusRegion regions[32];
  int count;
  BusRegion* last;  // one-entry FindRegion cache (validated by overlaps/base)
} Bus;

// Registers an MMIO device window (accesses go through ops->Read/Write).
void BusAddRegion(Bus* bus, uint64_t base, uint64_t size, const DeviceOps* ops, void* dev);
// Points an already-registered device window at a new address, adding one if the
// device has none yet. A PCI device's I/O window lives where firmware programs
// its BAR (PCI spec §6.2, and the base can be reprogrammed — seabios
// src/fw/pciinit.c runs again on a reboot path), so a device whose window is
// BAR-driven needs to move its decode rather than assume the one it started at.
void BusRelocateRegion(Bus* bus, const DeviceOps* ops, void* dev, uint64_t base, uint64_t size);
// Stops decoding a device window (the firmware disabled the device or zeroed its
// BAR: PCI spec §6.2.5.1 leaves a BAR written with 0 decoding nothing). Returns 1
// if the device had one.
int BusRemoveRegion(Bus* bus, const DeviceOps* ops, void* dev);
// Registers RAM with its host backing store for direct access.
void BusAddRamRegion(Bus* bus, uint64_t base, uint64_t size, const DeviceOps* ops, void* dev,
                     uint8_t* host);
int BusProbe(Bus* bus, uint64_t addr, int len, BusRegion** out);
uint64_t BusRead(Bus* bus, uint64_t addr, int size);
void BusWrite(Bus* bus, uint64_t addr, int size, uint64_t val);
// Returns the host pointer backing [addr, addr+len) inside a RAM region.
int BusRamRange(Bus* bus, uint64_t addr, uint64_t len, uint8_t** host);
// Page-granular RAM fast path for the CPU's translation cache: succeeds only
// when one non-overlapping RAM region backs the whole 4 KiB page at `addr`
// (page-aligned), so a direct host load/store needs no bus decode.
int BusRamPage(Bus* bus, uint64_t addr, uint8_t** host);

#endif
