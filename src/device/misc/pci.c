#include "device/misc/pci.h"

// The two halves of the legacy configuration mechanism (Intel 440FX datasheet
// §3.2; the same pair QEMU exposes as the pci-host config space).
enum {
  kPciAddrPort = 0xcf8,
  kPciDataPort = 0xcfc,
  kPciPortCount = 8,
  kPciEnable = 0x80000000u,
};

// Where a data-window access lands inside a device's configuration space:
// address-register bits 7:2 select the dword, the window's own low bits the
// byte inside it (Intel 440FX datasheet §3.2).
static uint32_t PciRegisterOffset(const PciBus* b, uint32_t window_off) {
  return (b->addr & 0xfc) | (window_off & 3);
}

static PciDevice* PciFind(PciBus* b, uint8_t bus, uint8_t dev, uint8_t func) {
  for (int i = 0; i < b->count; i++) {
    PciDevice* d = b->devices[i];
    if (d->bus == bus && d->dev == dev && d->func == func) return d;
  }
  return NULL;
}

static PciDevice* PciSelected(PciBus* b) {
  if (!(b->addr & kPciEnable)) return NULL;
  return PciFind(b, (uint8_t)(b->addr >> 16), (uint8_t)((b->addr >> 11) & 0x1f),
                 (uint8_t)((b->addr >> 8) & 7));
}

static uint64_t PciRead(void* dev, uint64_t addr, int size) {
  PciBus* b = (PciBus*)dev;
  if (addr < kPciDataPort) {
    // The address register is byte-addressable (bit 31 lives in the top byte).
    uint32_t v = b->addr;
    return (v >> (8 * (uint32_t)(addr - kPciAddrPort))) &
           (size >= 4 ? 0xffffffffu : (1u << (8 * size)) - 1);
  }
  PciDevice* d = PciSelected(b);
  uint32_t base = PciRegisterOffset(b, (uint32_t)(addr - kPciDataPort));
  uint64_t v = 0;
  for (int i = 0; i < size; i++)
    v |= (uint64_t)(d ? d->config[(base + (uint32_t)i) & 0xff] : 0xff) << (8 * i);
  return v;
}

static void PciWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  PciBus* b = (PciBus*)dev;
  if (addr < kPciDataPort) {
    // Store byte i at byte position (addr - 0xCF8 + i) of the 32-bit register.
    // Real hardware is byte-addressable and firmware programs it with one dword
    // write, which then lands whole — including bit 31, the enable bit.
    uint32_t off = (uint32_t)(addr - kPciAddrPort);
    for (int i = 0; i < size; i++) {
      uint32_t pos = off + (uint32_t)i;
      if (pos > 3) continue;
      uint32_t mask = 0xffu << (8 * pos);
      uint8_t byte = (uint8_t)((val >> (8 * i)) & 0xff);
      b->addr = (b->addr & ~mask) | ((uint32_t)byte << (8 * pos));
    }
    return;
  }
  PciDevice* d = PciSelected(b);
  if (!d) return;  // no device: the cycle goes nowhere
  uint32_t base = PciRegisterOffset(b, (uint32_t)(addr - kPciDataPort));
  for (int i = 0; i < size; i++) {
    uint8_t o = (uint8_t)((base + (uint32_t)i) & 0xff);
    uint8_t byte = (uint8_t)((val >> (8 * i)) & 0xff);
    d->config[o] = byte;
    if (d->written) d->written(d->ctx, o, byte);
  }
}

static const DeviceOps kPciOps = {"pci-config", PciRead, PciWrite};

void PciInit(PciBus* b) {
  b->addr = 0;
  b->count = 0;
}

void PciRegister(Bus* io, PciBus* b) { BusAddRegion(io, kPciAddrPort, kPciPortCount, &kPciOps, b); }

void PciAddDevice(PciBus* b, PciDevice* d) {
  if (b->count >= (int)ARRAY_SIZE(b->devices)) return;
  b->devices[b->count++] = d;
}
