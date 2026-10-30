#include "device/misc/portf0.h"

// Where the PC puts the IGNNE# latch (QEMU hw/i386/pc.c registers ioportF0_io
// at 0xF0, one byte).
enum { kPortF0 = 0xf0 };

static uint64_t PortF0Read(void* dev, uint64_t addr, int size) {
  (void)dev;
  (void)addr;
  return size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
}

static void PortF0Write(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)addr;
  (void)size;
  (void)val;  // see the header: the reference ignores the byte
  PortF0Device* d = (PortF0Device*)dev;
  if (d->set_ignne) d->set_ignne(d->ctx, 1);
}

static const DeviceOps kPortF0Ops = {"portf0", PortF0Read, PortF0Write};

void PortF0Init(PortF0Device* d) {
  d->set_ignne = NULL;
  d->ctx = NULL;
}

void PortF0Register(Bus* io, PortF0Device* d) { BusAddRegion(io, kPortF0, 1, &kPortF0Ops, d); }

void PortF0SetIgnneSink(PortF0Device* d, void (*set_ignne)(void* ctx, int on), void* ctx) {
  d->set_ignne = set_ignne;
  d->ctx = ctx;
}
