#include "device/intc/lapic.h"

// Register offsets (SDM vol.3 figure 11-18; one 32-bit register per 16
// bytes). Offsets outside the implemented set read 0 and drop writes — the
// reserved-MMIO behavior of a real LAPIC whose delivery is disabled.
enum {
  kLapicId = 0x20,
  kLapicVersion = 0x30,
  kLapicTpr = 0x80,
  kLapicEoi = 0xB0,
  kLapicError = 0x280,
  kLapicIcrLo = 0x300,
  kLapicIcrHi = 0x310,
};

static const uint64_t kLapicBase = 0xFEE00000ULL;
static const uint64_t kLapicSize = 0x1000ULL;

// Bits that read as one regardless of storage (SDM vol.3 11.4.4/11.4.8): the
// version register reports P6-class LVT count 5, the ICR delivery-status bit
// reads idle.
static uint64_t LapicRead(void* dev, uint64_t addr, int size) {
  (void)size;
  LapicDevice* d = (LapicDevice*)dev;
  uint64_t off = addr - kLapicBase;
  if (off & 0xf) return 0;
  uint32_t v = d->reg[off / 16];
  if (off == kLapicVersion) v = 0x50014;  // version 0x14, 5 LVT entries
  if (off == kLapicIcrLo) v &= ~(1u << 12);  // delivery status: idle
  return v;
}

static void LapicWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  LapicDevice* d = (LapicDevice*)dev;
  uint64_t off = addr - kLapicBase;
  if (off & 0xf) return;
  uint32_t v = (uint32_t)val;
  if (off == kLapicEoi) return;  // EOI: nothing is in service
  d->reg[off / 16] = v;
}

const DeviceOps kLapicOps = {"lapic", LapicRead, LapicWrite};

void LapicInit(LapicDevice* d) {
  for (int i = 0; i < (int)(sizeof(d->reg) / sizeof(d->reg[0])); i++) d->reg[i] = 0;
}

void LapicRegister(Bus* bus, LapicDevice* d) {
  BusAddRegion(bus, kLapicBase, kLapicSize, &kLapicOps, d);
}
