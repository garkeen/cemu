#include "bus/bus.h"

#include "util/log.h"

// Recomputes the "another region intersects me" flag the one-entry lookup
// cache consults, and drops that cache. Region sets change only at board
// build / BAR programming, never on the hot path.
static void RefreshBus(Bus* bus) {
  bus->last = NULL;
  for (int i = 0; i < bus->count; i++) {
    BusRegion* a = &bus->regions[i];
    a->overlaps = 0;
    for (int j = 0; j < bus->count; j++) {
      if (i == j) continue;
      BusRegion* b = &bus->regions[j];
      if (a->base < b->base + b->size && b->base < a->base + a->size) {
        a->overlaps = 1;
        break;
      }
    }
  }
}

void BusAddRegion(Bus* bus, uint64_t base, uint64_t size, const DeviceOps* ops, void* dev) {
  if (bus->count >= (int)ARRAY_SIZE(bus->regions))
    Fatal("bus full, cannot map %s at %llx", ops->name, (unsigned long long)base);
  BusRegion* r = &bus->regions[bus->count++];
  r->base = base;
  r->size = size;
  r->ops = ops;
  r->dev = dev;
  r->host = NULL;
  r->overlaps = 0;
  RefreshBus(bus);
}

void BusAddRamRegion(Bus* bus, uint64_t base, uint64_t size, const DeviceOps* ops, void* dev,
                     uint8_t* host) {
  BusAddRegion(bus, base, size, ops, dev);
  bus->regions[bus->count - 1].host = host;
}

void BusRelocateRegion(Bus* bus, const DeviceOps* ops, void* dev, uint64_t base, uint64_t size) {
  for (int i = 0; i < bus->count; i++) {
    if (bus->regions[i].ops == ops && bus->regions[i].dev == dev) {
      bus->regions[i].base = base;
      bus->regions[i].size = size;
      RefreshBus(bus);
      return;
    }
  }
  BusAddRegion(bus, base, size, ops, dev);
}

int BusRemoveRegion(Bus* bus, const DeviceOps* ops, void* dev) {
  for (int i = 0; i < bus->count; i++) {
    if (bus->regions[i].ops == ops && bus->regions[i].dev == dev) {
      for (int j = i + 1; j < bus->count; j++) bus->regions[j - 1] = bus->regions[j];
      bus->count--;
      RefreshBus(bus);
      return 1;
    }
  }
  return 0;
}

// Overlapping regions resolve to the smallest match, so a narrow device window
// takes precedence over the RAM backing it sits inside.
static BusRegion* FindRegion(Bus* bus, uint64_t addr, int len) {
  // A range wrapping 2^64 can never name one access: reject it up front
  // instead of letting addr+len overflow into a bogus match (a debugger
  // probing near the top of the address space, or a bare-metal guest access,
  // reaches this edge).
  if ((uint64_t)len > ~addr) return NULL;
  BusRegion* last = bus->last;
  if (last && !last->overlaps && addr >= last->base &&
      addr + len <= last->base + last->size)
    return last;
  BusRegion* best = NULL;
  for (int i = 0; i < bus->count; i++) {
    BusRegion* r = &bus->regions[i];
    if (addr >= r->base && addr + len <= r->base + r->size) {
      if (!best || r->size < best->size) best = r;
    }
  }
  bus->last = (best && !best->overlaps) ? best : NULL;
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

int BusRamPage(Bus* bus, uint64_t addr, uint8_t** host) {
  if (addr & 0xfff) return -1;
  for (int i = 0; i < bus->count; i++) {
    BusRegion* r = &bus->regions[i];
    if (r->host && !r->overlaps && addr >= r->base && addr + 0x1000 <= r->base + r->size) {
      *host = r->host + (addr - r->base);
      return 0;
    }
  }
  return -1;
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
