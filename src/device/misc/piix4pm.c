#include "device/misc/piix4pm.h"

#include <string.h>

#include "host/host.h"

// PCI configuration offsets and this function's identity (PCI spec §6.1;
// seabios src/hw/pci_ids.h PCI_DEVICE_ID_INTEL_82371AB_3 = 0x7113,
// src/fw/dev-piix.h for PIIX_PMBASE and PIIX_PMREGMISC).
enum {
  kCfgVendor = 0x00,
  kCfgDevice = 0x02,
  kCfgRevision = 0x08,
  kCfgProgIf = 0x09,
  kCfgSubClass = 0x0a,
  kCfgClass = 0x0b,
  kCfgIntPin = 0x3d,
  kCfgPmBase = 0x40,
  kCfgPmRegMisc = 0x80,
};
enum {
  kVendorIntel = 0x8086,
  kDevicePiix4Pm = 0x7113,
  // QEMU's -machine pc reports this function at revision 3.
  kRevision = 3,
  kProgIf = 0,
  // Class 0x0680: bridge, other — what `info pci` prints as "Bridge".
  kSubClassOther = 0x80,
  kClassBridge = 0x06,
  kIntPinA = 1,
};

// The PM register block. Its size and the offsets inside it come from what
// firmware programs and reads: seabios src/fw/pciinit.c piix4_pm_config_setup
// writes the block base at PIIX_PMBASE with the I/O-space indicator in bit 0 and
// turns the block on with PIIX_PMREGMISC bit 0 ("enable PM io space"), and
// src/hw/timer.c pmtimer_setup(acpi_pm_base + 0x08) reads the timer there while
// src/fw/pciinit.c takes PM1a control at +0x04.
enum {
  kPmBlockSize = 32,
  kPmTimerOffset = 0x08,
  kPmIoSpaceEnable = 0x01,  // PIIX_PMREGMISC bit 0
  kPmBaseIoIndicator = 0x01,
};

// The timer's rate: 3.579545 MHz, the 32.768 kHz watch crystal times 109.2 —
// the frequency ACPI defines for the PM timer and the one seabios src/hw/timer.c
// PMTIMER_HZ states, from which it derives every microsecond the firmware waits.
static const uint64_t kPmTimerHz = 3579545;
static const uint64_t kMicroPerSec = 1000000;

static uint32_t PmTicks(void) {
  return (uint32_t)((uint64_t)HostTimerNow() * kPmTimerHz / kMicroPerSec);
}

static uint64_t PmRead(void* dev, uint64_t addr, int size) {
  Piix4PmDevice* d = (Piix4PmDevice*)dev;
  uint64_t off = addr - (uint64_t)d->base;
  // Only the timer holds anything. PM1a status/event and PM1a control are the
  // SCI path this machine does not build yet (简化登记 D19), and a status
  // register with no event set reads as no event.
  if (off < kPmTimerOffset || off >= kPmTimerOffset + 4) return 0;
  uint64_t mask = (size >= 8) ? ~0ull : ((1ull << (8 * size)) - 1);
  return (PmTicks() >> (8 * (off - kPmTimerOffset))) & mask;
}

static void PmWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)dev;
  (void)addr;
  (void)size;
  (void)val;
  // PM1a control would end the guest's session (ACPI shutdown) and the PM1a
  // event bits would raise the ACPI SCI on IRQ9. Neither path is modelled —
  // 简化登记 D19.
}

static const DeviceOps kPmOps = {"piix4pm", PmRead, PmWrite};

// Repoints the block whenever firmware moves PMBASE or gates it with
// PIIX_PMREGMISC bit 0 — the window a PCI device decodes lives where its base
// register says, and firmware can reprogram it (it does so again on the reboot
// path, seabios src/fw/pciinit.c:387).
static void PmMap(Piix4PmDevice* d) {
  uint32_t reg = (uint32_t)d->pci.config[kCfgPmBase] |
                 ((uint32_t)d->pci.config[kCfgPmBase + 1] << 8);
  uint16_t base = (uint16_t)(reg & ~(uint32_t)(kPmBlockSize - 1));
  // A zero base cannot name a window: PMBASE bit 0 selects I/O space and bits
  // 6:1 are reserved, but the address field must be non-zero for the block to
  // decode (otherwise enabling it would shadow the DMA controller at 0x0000).
  int on = (d->pmregmisc & kPmIoSpaceEnable) && (reg & kPmBaseIoIndicator) && base != 0;
  d->base = on ? base : 0;
  if (on)
    BusRelocateRegion(d->io, &kPmOps, d, base, kPmBlockSize);
  else
    BusRemoveRegion(d->io, &kPmOps, d);
}

static void ConfigWritten(void* ctx, uint8_t off, uint8_t val) {
  Piix4PmDevice* d = (Piix4PmDevice*)ctx;
  // Firmware programs the base first and enables the block afterwards (seabios
  // src/fw/pciinit.c piix4_pm_config_setup writes PIIX_PMBASE, then
  // PIIX_PMREGMISC), so either register moves the decode.
  if (off == kCfgPmRegMisc) d->pmregmisc = val;
  if (off == kCfgPmBase || off == kCfgPmBase + 1 || off == kCfgPmRegMisc) PmMap(d);
}

static void Put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)(v >> 8);
}

// What the function answers before any firmware has touched it.
static void FillIdentity(Piix4PmDevice* d) {
  Put16(&d->pci.config[kCfgVendor], kVendorIntel);
  Put16(&d->pci.config[kCfgDevice], kDevicePiix4Pm);
  d->pci.config[kCfgRevision] = kRevision;
  d->pci.config[kCfgProgIf] = kProgIf;
  d->pci.config[kCfgSubClass] = kSubClassOther;
  d->pci.config[kCfgClass] = kClassBridge;
  d->pci.config[kCfgIntPin] = kIntPinA;
}

void Piix4PmInit(Piix4PmDevice* d, uint8_t bus, uint8_t dev) {
  memset(d, 0, sizeof(*d));
  d->pci.bus = bus;
  d->pci.dev = dev;
  // Function 3, where QEMU puts this device (its `info pci` shows 8086:7113 at
  // bus 0 device 1 function 3); firmware reaches it because the ISA bridge at
  // function 0 declares the device multi-function.
  d->pci.func = 3;
  d->pci.written = ConfigWritten;
  d->pci.ctx = d;
  FillIdentity(d);
}

void Piix4PmSetIoBus(Piix4PmDevice* d, Bus* io) { d->io = io; }

// A reset puts the block back to unprogrammed and off, which is how the chip
// starts: nothing is decoded until firmware names the base and enables it. The
// vendor base register itself returns to its reset value too (0); leaving the
// old address there would make the next enumeration read back a base the chip
// is not decoding.
void Piix4PmReset(Piix4PmDevice* d) {
  FillIdentity(d);
  d->pci.config[kCfgPmBase] = 0;
  d->pci.config[kCfgPmBase + 1] = 0;
  d->pci.config[kCfgPmRegMisc] = 0;
  d->pmregmisc = 0;
  d->base = 0;
  BusRemoveRegion(d->io, &kPmOps, d);
}
