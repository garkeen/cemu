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

void Piix3BridgeInit(Piix3BridgeDevice* d, uint8_t bus, uint8_t dev) {
  memset(d, 0, sizeof(*d));
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
