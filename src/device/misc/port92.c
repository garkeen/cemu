#include "device/misc/port92.h"

// Port 0x92 bits (PC/AT Technical Reference; QEMU hw/i386/port92.c).
enum { kPort92Reset = 0x01, kPort92A20 = 0x02 };

static uint64_t Port92Read(void* dev, uint64_t addr, int size) {
  (void)addr;
  (void)size;
  return ((Port92Device*)dev)->outport;
}

static void Port92Write(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)addr;
  (void)size;
  Port92Device* d = (Port92Device*)dev;
  uint8_t prev = d->outport;
  d->outport = (uint8_t)val;
  // Bit 0 is the "fast reset" line (INIT_RC); the CPU reset facility is not
  // modeled yet (AGENTS.md D18), so it is stored and ignored.
  if ((prev ^ d->outport) & kPort92A20 && d->set_a20)
    d->set_a20(d->a20_ctx, (d->outport & kPort92A20) ? 1 : 0);
}

static const DeviceOps kPort92Ops = {"port92", Port92Read, Port92Write};

void Port92Init(Port92Device* d) {
  d->outport = kPort92A20;
  d->set_a20 = NULL;
  d->a20_ctx = NULL;
}

void Port92Register(Bus* io, Port92Device* d) { BusAddRegion(io, 0x92, 1, &kPort92Ops, d); }

void Port92SetA20Sink(Port92Device* d, void (*set_a20)(void* ctx, int on), void* ctx) {
  d->set_a20 = set_a20;
  d->a20_ctx = ctx;
}
