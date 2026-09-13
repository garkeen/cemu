#include "bus/bus.h"

#include "util/log.h"

void BusAddRegion(Bus* bus, uint64_t base, uint64_t size, const DeviceOps* ops, void* dev) {
  if (bus->count >= (int)ARRAY_SIZE(bus->regions))
    Fatal("bus full, cannot map %s at %llx", ops->name, (unsigned long long)base);
  BusRegion* r = &bus->regions[bus->count++];
  r->base = base;
  r->size = size;
  r->ops = ops;
  r->dev = dev;
  r->host = NULL;
}

void BusAddRamRegion(Bus* bus, uint64_t base, uint64_t size, const DeviceOps* ops, void* dev,
                     uint8_t* host) {
  BusAddRegion(bus, base, size, ops, dev);
  bus->regions[bus->count - 1].host = host;
}

// Overlapping regions resolve to the smallest match, so a narrow device window
// takes precedence over the RAM backing it sits inside.
static BusRegion* FindRegion(Bus* bus, uint64_t addr, int len) {
  // A range wrapping 2^64 can never name one access: reject it up front
  // instead of letting addr+len overflow into a bogus match (a debugger
  // probing near the top of the address space, or a bare-metal guest access,
  // reaches this edge).
  if ((uint64_t)len > ~addr) return NULL;
  BusRegion* best = NULL;
  for (int i = 0; i < bus->count; i++) {
    BusRegion* r = &bus->regions[i];
    if (addr >= r->base && addr + len <= r->base + r->size) {
      if (!best || r->size < best->size) best = r;
    }
  }
  return best;
}

int BusProbe(Bus* bus, uint64_t addr, int len, BusRegion** out) {
  BusRegion* r = FindRegion(bus, addr, len);
  if (out) *out = r;
  return r ? 0 : -1;
}

uint64_t BusRead(Bus* bus, uint64_t addr, int size) {
  BusRegion* r = FindRegion(bus, addr, size);
  if (!r) Fatal("read of unmapped address %llx", (unsigned long long)addr);
  return r->ops->Read(r->dev, addr, size);
}

void BusWrite(Bus* bus, uint64_t addr, int size, uint64_t val) {
  BusRegion* r = FindRegion(bus, addr, size);
  if (!r) Fatal("write of unmapped address %llx", (unsigned long long)addr);
  r->ops->Write(r->dev, addr, size, val);
}

int BusRamRange(Bus* bus, uint64_t addr, uint64_t len, uint8_t** host) {
  // Smallest match, like FindRegion: a device RAM window sitting inside
  // board RAM (e.g. the CGA frame buffer at 0xB8000) owns its bytes.
  BusRegion* best = NULL;
  for (int i = 0; i < bus->count; i++) {
    BusRegion* r = &bus->regions[i];
    if (r->host && addr >= r->base && addr + len <= r->base + r->size) {
      if (!best || r->size < best->size) best = r;
    }
  }
  if (!best) return -1;
  *host = best->host + (addr - best->base);
  return 0;
}
