#include "device/misc/i440fx.h"

#include <string.h>

// Configuration offsets (Intel 82441FX datasheet §4.1) and the identity QEMU
// gives the same part (hw/pci-host/i440fx.c: 8086:1237, class 0x0600 host
// bridge, revision 2; seabios src/hw/pci_ids.h PCI_DEVICE_ID_INTEL_82441).
enum {
  kCfgVendor = 0x00,
  kCfgDevice = 0x02,
  kCfgRevision = 0x08,
  kCfgClass = 0x0a,
  kCfgHeaderType = 0x0e,
  kPam0 = 0x59,
};
enum { kVendorIntel = 0x8086, kDevice82441 = 0x1237, kClassHostBridge = 0x0600, kRevision = 2 };

// PAM windows: PAM1..6 hold 32KiB each from 0xC0000, PAM0 holds the whole
// F-segment. Each 32KiB register is two 16KiB halves (Intel 82441FX §4.1).
static const uint64_t kPamBase = 0xc0000;
static const uint64_t kPamRegion = 0x8000;
static const uint64_t kPamFBase = 0xf0000;
enum { kPamWriteLow = 0x02, kPamWriteHigh = 0x20 };

static void Put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)(v >> 8);
}

void I440fxInit(I440fxDevice* d, uint8_t bus, uint8_t dev) {
  memset(&d->pci, 0, sizeof(d->pci));
  d->pci.bus = bus;
  d->pci.dev = dev;
  d->pci.func = 0;
  Put16(&d->pci.config[kCfgVendor], kVendorIntel);
  Put16(&d->pci.config[kCfgDevice], kDevice82441);
  d->pci.config[kCfgRevision] = kRevision;
  Put16(&d->pci.config[kCfgClass], kClassHostBridge);
  d->pci.config[kCfgHeaderType] = 0;  // single function, type 0 header
}

int I440fxRamWritable(const I440fxDevice* d, uint64_t addr) {
  if (addr >= kPamFBase) return (d->pci.config[kPam0] & kPamWriteHigh) != 0;
  if (addr < kPamBase) return 0;
  uint64_t off = addr - kPamBase;
  uint8_t pam = d->pci.config[kPam0 + 1 + (int)(off / kPamRegion)];
  int high_half = (off % kPamRegion) >= kPamRegion / 2;
  return (pam & (high_half ? kPamWriteHigh : kPamWriteLow)) != 0;
}
