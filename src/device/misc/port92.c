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
  // Bit 0 is INIT_NOW: the fast-reset line, which the machine answers with a
  // reset (QEMU hw/i386/port92.c does the same on a write with bit 0 set). The
  // request is recorded by the board and taken at the next instruction
  // boundary — this write is still inside the instruction that issued it.
  if ((d->outport & kPort92Reset) && d->request_reset) d->request_reset(d->reset_ctx);
  if ((prev ^ d->outport) & kPort92A20 && d->set_a20)
    d->set_a20(d->a20_ctx, (d->outport & kPort92A20) ? 1 : 0);
}

static const DeviceOps kPort92Ops = {"port92", Port92Read, Port92Write};

void Port92Init(Port92Device* d) {
  d->outport = kPort92A20;
  d->set_a20 = NULL;
  d->a20_ctx = NULL;
  d->request_reset = NULL;
  d->reset_ctx = NULL;
}

void Port92Register(Bus* io, Port92Device* d) { BusAddRegion(io, 0x92, 1, &kPort92Ops, d); }

void Port92SetA20Sink(Port92Device* d, void (*set_a20)(void* ctx, int on), void* ctx) {
  d->set_a20 = set_a20;
  d->a20_ctx = ctx;
}

void Port92SetResetSink(Port92Device* d, void (*request_reset)(void* ctx), void* ctx) {
  d->request_reset = request_reset;
  d->reset_ctx = ctx;
}
