#include "device/misc/testdev.h"

// Every width lands on the same byte: v86 registers the one handler for the
// 8/16/32-bit writes (src/cpu.js register_write).
static void TestDevWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  TestDevDevice* d = (TestDevDevice*)dev;
  // addr is the absolute port (the bus hands the device its own address, like
  // cmos.c comparing against 0x70/0x71).
  int line = (int)(addr - kTestDevBase);
  if (line < 0 || line >= kTestDevLines) return;
  if (d->set_irq) d->set_irq(d->irq_ctx, line, (val & 0xff) != 0);
}

static uint64_t TestDevRead(void* dev, uint64_t addr, int size) {
  (void)dev;
  (void)addr;
  (void)size;
  return 0;
}

const DeviceOps kTestDevOps = {"testdev", TestDevRead, TestDevWrite};

void TestDevInit(TestDevDevice* d) {
  d->set_irq = NULL;
  d->irq_ctx = NULL;
}

void TestDevRegister(Bus* io, TestDevDevice* d) {
  BusAddRegion(io, kTestDevBase, kTestDevLines, &kTestDevOps, d);
}

void TestDevSetIrqSink(TestDevDevice* d, void (*set_irq)(void* ctx, int line, int level), void* ctx) {
  d->set_irq = set_irq;
  d->irq_ctx = ctx;
}
