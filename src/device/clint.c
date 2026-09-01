#include "device/clint.h"
#include "host/host.h"

// Register offsets inside the CLINT window (SiFive CLINT manual; QEMU virt
// DTB "sifive,clint0").
enum {
  kClintOffMsip = 0x0,
  kClintOffMtimecmp = 0x4000,
  kClintOffMtime = 0xBFF8,
};

static uint64_t SubRead64(uint64_t v, uint64_t off, int size) {
  uint64_t mask = size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
  return (v >> (off * 8)) & mask;
}

static uint64_t SubWrite64(uint64_t old, uint64_t off, int size, uint64_t val) {
  uint64_t mask = size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
  return (old & ~(mask << (off * 8))) | ((val & mask) << (off * 8));
}

void ClintInit(ClintDevice *c, uint64_t timebase_hz) {
  c->msip = 0;
  c->mtimecmp = ~0ULL;
  c->timebase_hz = timebase_hz;
  c->mtime0 = 0;
  c->host0 = (uint64_t)HostTimerNow();
}

uint64_t ClintMtime(ClintDevice *c) {
  uint64_t host = (uint64_t)HostTimerNow();
  return c->mtime0 + (host - c->host0) * c->timebase_hz / 1000000;
}

static void UpdateMtip(ClintDevice *c) {
  int level = ClintMtime(c) >= c->mtimecmp;
  if (c->set_irq) c->set_irq(c->irq_ctx, kClintLineMtip, level);
}

void ClintPoll(ClintDevice *c) { UpdateMtip(c); }

static void UpdateMsip(ClintDevice *c) {
  if (c->set_irq) c->set_irq(c->irq_ctx, kClintLineMsip, c->msip & 1);
}

static uint64_t ClintRead(void *dev, uint64_t addr, int size) {
  ClintDevice *c = (ClintDevice *)dev;
  if (addr >= c->base + kClintOffMsip &&
      addr < c->base + kClintOffMsip + 4)
    return c->msip & 1;
  if (addr >= c->base + kClintOffMtimecmp &&
      addr < c->base + kClintOffMtimecmp + 8)
    return SubRead64(c->mtimecmp, addr - (c->base + kClintOffMtimecmp), size);
  if (addr >= c->base + kClintOffMtime &&
      addr < c->base + kClintOffMtime + 8)
    return SubRead64(ClintMtime(c), addr - (c->base + kClintOffMtime), size);
  return 0;  // undecoded reads as zero (QEMU mmio default)
}

static void ClintWrite(void *dev, uint64_t addr, int size, uint64_t val) {
  ClintDevice *c = (ClintDevice *)dev;
  if (addr >= c->base + kClintOffMsip &&
      addr < c->base + kClintOffMsip + 4) {
    c->msip = (uint32_t)(val & 1);
    UpdateMsip(c);
    return;
  }
  if (addr >= c->base + kClintOffMtimecmp &&
      addr < c->base + kClintOffMtimecmp + 8) {
    c->mtimecmp = SubWrite64(c->mtimecmp, addr - (c->base + kClintOffMtimecmp),
                             size, val);
    // Writing mtimecmp retires a pending MTIP (dearchap clint_write).
    UpdateMtip(c);
    return;
  }
  if (addr >= c->base + kClintOffMtime &&
      addr < c->base + kClintOffMtime + 8) {
    uint64_t m = ClintMtime(c);
    m = SubWrite64(m, addr - (c->base + kClintOffMtime), size, val);
    c->mtime0 = m;
    c->host0 = (uint64_t)HostTimerNow();
    UpdateMtip(c);
    return;
  }
  // undecoded writes are dropped
}

static const DeviceOps kClintOps = {"clint", ClintRead, ClintWrite};

void ClintRegister(Bus *bus, ClintDevice *c, uint64_t base, uint64_t size) {
  c->base = base;
  BusAddRegion(bus, base, size, &kClintOps, c);
}

void ClintSetIrqSink(ClintDevice *c, void (*set_irq)(void *, int, int),
                     void *ctx) {
  c->set_irq = set_irq;
  c->irq_ctx = ctx;
}
