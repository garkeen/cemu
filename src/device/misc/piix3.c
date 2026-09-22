#include "device/misc/piix3.h"

#include <string.h>

// PCI configuration offsets and the identity of the ISA bridge function
// (PCI spec §6.1; seabios src/hw/pci_ids.h, src/hw/pci.c).
enum {
  kCfgVendor = 0x00,
  kCfgDevice = 0x02,
  kCfgRevision = 0x08,
  kCfgProgIf = 0x09,
  kCfgSubClass = 0x0a,
  kCfgClass = 0x0b,
  kCfgHeaderType = 0x0e,
};
enum {
  kVendorIntel = 0x8086,
  kDevicePiix3Isa = 0x7000,  // PCI_DEVICE_ID_INTEL_82371SB_0
  kRevision = 0,
  kProgIf = 0,  // no programming interface defined for the ISA bridge
  kSubClassIsa = 0x01,
  kClassBridge = 0x06,
  // Bit 7 marks a multi-function device: firmware only looks at function 1 when
  // function 0 says so (seabios src/hw/pci.c pci_next).
  kHeaderTypeMultiFunction = 0x80,
};

static void Put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)(v >> 8);
}

// The reset control register at I/O port 0xCF9 (PIIX3 datasheet; QEMU
// hw/isa/piix3.c rcr_write): bit 1 is the reset *type* (0 = soft INIT, 1 =
// hard reset) and stays in the register, bit 2 is the request — a write that
// sets it resets the machine. SeaBIOS's pci_reboot() is the guest this serves:
// it writes `v|2` and then `v|6`, and the second write is the one that acts.
enum {
  kRcrPort = 0xcf9,
  kRcrResetType = 0x02,
  kRcrResetRequest = 0x04,
};

static uint64_t RcrRead(void* dev, uint64_t addr, int size) {
  (void)addr;
  (void)size;
  return ((Piix3BridgeDevice*)dev)->rcr;
}

static void RcrWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)addr;
  (void)size;
  Piix3BridgeDevice* d = (Piix3BridgeDevice*)dev;
  if (val & kRcrResetRequest) {
    // The board records the request and resets at the next instruction
    // boundary; this write is still inside the instruction that issued it.
    if (d->request_reset) d->request_reset(d->reset_ctx);
    return;
  }
  d->rcr = (uint8_t)(val & kRcrResetType);
}

static const DeviceOps kRcrOps = {"piix3-rcr", RcrRead, RcrWrite};

void Piix3Register(Bus* io, Piix3BridgeDevice* d) { BusAddRegion(io, kRcrPort, 1, &kRcrOps, d); }

void Piix3SetResetSink(Piix3BridgeDevice* d, void (*request_reset)(void* ctx), void* ctx) {
  d->request_reset = request_reset;
  d->reset_ctx = ctx;
}

void Piix3BridgeInit(Piix3BridgeDevice* d, uint8_t bus, uint8_t dev) {
  memset(d, 0, sizeof(*d));  // also the RCR and the reset sink
  d->pci.bus = bus;
  d->pci.dev = dev;
  d->pci.func = 0;
  Put16(&d->pci.config[kCfgVendor], kVendorIntel);
  Put16(&d->pci.config[kCfgDevice], kDevicePiix3Isa);
  d->pci.config[kCfgRevision] = kRevision;
  d->pci.config[kCfgProgIf] = kProgIf;
  d->pci.config[kCfgSubClass] = kSubClassIsa;
  d->pci.config[kCfgClass] = kClassBridge;
  d->pci.config[kCfgHeaderType] = kHeaderTypeMultiFunction;
}
