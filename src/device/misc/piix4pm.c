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

// The fixed-event bits the PM1a block implements (ACPI 4.8): the timer
// overflow, the global-lock release, the power button, the RTC alarm and the
// wake status. PM1_STS is write-one-to-clear; PM1_EN has the same bit
// positions and gates those events onto the SCI.
enum {
  kPm1StsTimer = 0x0001,
  kPm1StsGlobalLock = 0x0020,
  kPm1StsPowerButton = 0x0100,
  kPm1StsRtc = 0x0400,
  kPm1StsWake = 0x8000,
  kPm1StatusMask = kPm1StsTimer | kPm1StsGlobalLock | kPm1StsPowerButton |
                   kPm1StsRtc | kPm1StsWake,
  kPm1EnMask = kPm1StsTimer | kPm1StsGlobalLock | kPm1StsPowerButton | kPm1StsRtc,
  // PM1_CNT: SCI_EN, BM_RLD, GBL_RLS, SLP_TYP (bits 12:10) and SLP_EN (13).
  kPm1CntSciEn = 0x0001,
  kPm1CntBmRld = 0x0002,
  kPm1CntGlobalRelease = 0x0004,
  kPm1CntSleepType = 0x1c00,
  kPm1CntSleepEnable = 0x2000,
  kPm1SleepTypeShift = 10,
  // S5 is the soft-off state; S3/S4 need a wake path this machine does not
  // have (简化登记 D19).
  kPm1SleepS5 = 5,
  // The PIIX4 PM function's SCI is the machine's IRQ9 (QEMU piix4.c's
  // sci_int default and the value SeaBIOS's FADT names).
  kSciIrq = 9,
};

static void PmUpdateSci(Piix4PmDevice* d) {
  int level = (d->pm1_cnt & kPm1CntSciEn) && (d->pm1_sts & d->pm1_en) != 0;
  if (d->set_sci) d->set_sci(d->sci_ctx, kSciIrq, level);
}

static uint8_t PmReadByte(const Piix4PmDevice* d, uint64_t off) {
  switch (off) {
    case 0:
    case 1:
      return (uint8_t)(d->pm1_sts >> (8 * off));
    case 2:
    case 3:
      return (uint8_t)(d->pm1_en >> (8 * (off - 2)));
    case 4:
    case 5:
      return (uint8_t)(d->pm1_cnt >> (8 * (off - 4)));
    default:
      break;
  }
  if (off >= kPmTimerOffset && off < kPmTimerOffset + 4)
    return (uint8_t)(PmTicks() >> (8 * (off - kPmTimerOffset)));
  return 0;
}

static uint64_t PmRead(void* dev, uint64_t addr, int size) {
  Piix4PmDevice* d = (Piix4PmDevice*)dev;
  const uint64_t off = addr - (uint64_t)d->base;
  uint64_t v = 0;
  for (int i = 0; i < size; i++)
    v |= (uint64_t)PmReadByte(d, off + (uint64_t)i) << (8 * i);
  return v;
}

// A PM1_CNT write's action bits are edges: GBL_RLS sets GBL_STS and does not
// stay set, and SLP_EN asks the board to enter the named sleep state. SCI_EN
// gates every fixed event onto the SCI (ACPI 4.8; QEMU hw/acpi's
// acpi_pm1_cnt_write/acpi_update_sci).
static void PmWriteControl(Piix4PmDevice* d, uint16_t v) {
  if (v & kPm1CntGlobalRelease) d->pm1_sts |= kPm1StsGlobalLock;
  d->pm1_cnt = (uint16_t)(v & ~kPm1CntGlobalRelease);
  if (d->pm1_cnt & kPm1CntSleepEnable) {
    const int type = (d->pm1_cnt & kPm1CntSleepType) >> kPm1SleepTypeShift;
    if (type == kPm1SleepS5 && d->request_shutdown) d->request_shutdown(d->shutdown_ctx);
  }
  PmUpdateSci(d);
}

static void PmWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  Piix4PmDevice* d = (Piix4PmDevice*)dev;
  const uint64_t off = addr - (uint64_t)d->base;
  for (int i = 0; i < size; i++) {
    const uint8_t b = (uint8_t)(val >> (8 * i));
    const uint64_t o = off + (uint64_t)i;
    const uint16_t v = (uint16_t)(b << (8 * (o & 1)));
    if (o == 0 || o == 1) {
      // Write one to clear a fixed-event bit (ACPI 4.8 PM1_STS).
      d->pm1_sts &= (uint16_t)~(v & kPm1StatusMask);
      PmUpdateSci(d);
    } else if (o == 2 || o == 3) {
      d->pm1_en = (uint16_t)((d->pm1_en & ~(uint16_t)(0xffu << (8 * (o & 1)))) |
                             (v & kPm1EnMask));
      PmUpdateSci(d);
    } else if (o == 4 || o == 5) {
      const uint16_t merged =
          (uint16_t)((d->pm1_cnt & ~(uint16_t)(0xffu << (8 * (o & 1)))) | v);
      PmWriteControl(d, merged);
    }
  }
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
  d->pm1_sts = 0;
  d->pm1_en = 0;
  d->pm1_cnt = 0;
  d->last_timer = PmTicks();
  if (d->set_sci) d->set_sci(d->sci_ctx, kSciIrq, 0);
  BusRemoveRegion(d->io, &kPmOps, d);
}

void Piix4PmSetIrqSink(Piix4PmDevice* d, void (*set_sci)(void* ctx, int line, int level),
                       void* ctx) {
  d->set_sci = set_sci;
  d->sci_ctx = ctx;
}

void Piix4PmSetShutdownSink(Piix4PmDevice* d, void (*request_shutdown)(void* ctx),
                            void* ctx) {
  d->request_shutdown = request_shutdown;
  d->shutdown_ctx = ctx;
}

// The PM timer's overflow is a fixed event: bit 0 of PM1_STS and, when enabled
// and SCI_EN is set, an SCI. The counter is free-running, so the wrap is
// detected by comparing samples of the same 32-bit value the guest reads.
void Piix4PmPoll(Piix4PmDevice* d) {
  const uint32_t t = PmTicks();
  if (t < d->last_timer) {
    d->pm1_sts |= kPm1StsTimer;
    PmUpdateSci(d);
  }
  d->last_timer = t;
}
