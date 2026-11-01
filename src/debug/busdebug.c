#include "debug/busdebug.h"

#include <stdlib.h>
#include <string.h>

#include "cpu/step.h"
#include "debug/debug.h"

typedef struct BusDebugRegion {
  const DeviceOps* real_ops;
  void* real_dev;
  uint8_t* real_host;
  uint64_t real_base;
  int is_fetch;
} BusDebugRegion;

struct BusDebug {
  Bus* real;
  uint32_t real_version;
  Bus data;
  Bus fetch;
  BusDebugRegion data_region[32];
  BusDebugRegion fetch_region[32];
};

static uint64_t BusDebugRead(void* dev, uint64_t addr, int size) {
  BusDebugRegion* w = (BusDebugRegion*)dev;
  uint64_t v = 0;
  if (w->real_host) {
    memcpy(&v, w->real_host + (addr - w->real_base), (size_t)size);
  } else {
    v = w->real_ops->Read(w->real_dev, addr, size);
  }
  frame* f = DebugFrame();
  if (w->is_fetch && f) DebugFetchByte((uint8_t)v);
  if (f) {
    // bus= excludes instruction fetch: firmware executing from its ROM window
    // would otherwise fill the session's output before any device access.
    if (!w->is_fetch && !w->real_host && DebugOn(kDbgBus))
      DebugBus(f, w->real_ops->name, addr, size, 1, v);
    if (DebugOn(kDbgMem)) DebugMem(f, addr, addr, size, acc_read, v, 1);
    if (!w->is_fetch && DebugLinWatchActive() && f->cpu->phys_to_lin) {
      int ok = 0;
      uint64_t lin = f->cpu->phys_to_lin(f->cpu->priv, addr, &ok);
      if (ok) DebugLinWatch(f, lin, size, 1, v);
    }
  }
  return v;
}

static void BusDebugWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  BusDebugRegion* w = (BusDebugRegion*)dev;
  frame* f = DebugFrame();
  if (f) {
    if (!w->is_fetch && !w->real_host && DebugOn(kDbgBus))
      DebugBus(f, w->real_ops->name, addr, size, 0, val);
    else if (DebugOn(kDbgMem))
      DebugMem(f, addr, addr, size, acc_write, val, 0);
  }
  if (w->real_host) {
    memcpy(w->real_host + (addr - w->real_base), &val, (size_t)size);
  } else {
    w->real_ops->Write(w->real_dev, addr, size, val);
  }
  // A store row is emitted after the store landed, as lin_store did: a
  // faulting store must not look like it reached memory.
  if (f && !w->is_fetch && DebugLinWatchActive() && f->cpu->phys_to_lin) {
    int ok = 0;
    uint64_t lin = f->cpu->phys_to_lin(f->cpu->priv, addr, &ok);
    if (ok) DebugLinWatch(f, lin, size, 0, val);
  }
}

static const DeviceOps kBusDebugOps = {"busdebug", BusDebugRead, BusDebugWrite};

static void AddWrapper(Bus* bus, BusDebugRegion* w, const BusRegion* real, int is_fetch) {
  w->real_ops = real->ops;
  w->real_dev = real->dev;
  w->real_host = real->host;
  w->real_base = real->base;
  w->is_fetch = is_fetch;
  BusRegion* r = &bus->regions[bus->count++];
  r->base = real->base;
  r->size = real->size;
  r->ops = &kBusDebugOps;
  r->dev = w;
  r->host = NULL;  // no direct host path: every access goes through the wrapper
  r->overlaps = real->overlaps;
}

void BusDebugSync(BusDebug* d) {
  if (!d || d->real_version == d->real->version) return;
  d->real_version = d->real->version;
  d->data.count = 0;
  d->fetch.count = 0;
  d->data.last = NULL;
  d->fetch.last = NULL;
  for (int i = 0; i < d->real->count; i++) {
    AddWrapper(&d->data, &d->data_region[i], &d->real->regions[i], 0);
    AddWrapper(&d->fetch, &d->fetch_region[i], &d->real->regions[i], 1);
  }
}

BusDebug* BusDebugCreate(Bus* real) {
  BusDebug* d = (BusDebug*)calloc(1, sizeof(*d));
  if (!d) return NULL;
  d->real = real;
  d->real_version = real->version + 1;  // force the first sync
  BusDebugSync(d);
  return d;
}

void BusDebugDestroy(BusDebug* d) { free(d); }

Bus* BusDebugDataBus(BusDebug* d) { return &d->data; }
Bus* BusDebugFetchBus(BusDebug* d) { return &d->fetch; }
