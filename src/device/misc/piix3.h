#ifndef CEMU_DEVICE_MISC_PIIX3_H
#define CEMU_DEVICE_MISC_PIIX3_H

#include <stdint.h>

#include "device/misc/pci.h"

// Intel 82371SB (PIIX3) PCI-to-ISA bridge, the half of the chipset that is not
// the IDE function: device 1 function 0 on bus 0, 8086:7000 (seabios
// src/hw/pci_ids.h PCI_DEVICE_ID_INTEL_82371SB_0), class 0x0601 (bridge, ISA).
//
// Its presence is what makes the IDE function visible at all: firmware walks
// the bus function by function and stops at the last function of a device whose
// function 0 does not have bit 7 of the header-type register set (seabios
// src/hw/pci.c pci_next; the PCI spec's multi-function device rule), so without
// this function firmware would never look at 00:01.1.
//
// Configuration space beyond the identity is plain register storage — the
// chipset's PIRQ routing bytes at 0x60..0x63, which firmware programs from its
// IRQ tables (seabios src/fw/pciinit.c piix_isa_bridge_setup), are written and
// read back by the generic PCI transport. Nothing routes an interrupt through
// them yet: the IDE function stays in compatibility mode, so the board's
// devices use the fixed ISA IRQs. The remaining PIIX3 registers (the XBCS BIOS
// control byte, the DMA and PM blocks) are not modelled — see 简化登记 D19.
//
// The one I/O port the bridge owns is the reset control register at 0xCF9.
typedef struct Piix3BridgeDevice {
  PciDevice pci;
  uint8_t rcr;  // reset control register: the reset type bit, once written
  // A write that requests a reset asks the machine for one (D18); the board
  // records it and acts at the next instruction boundary.
  void (*request_reset)(void* ctx);
  void* reset_ctx;
} Piix3BridgeDevice;

void Piix3BridgeInit(Piix3BridgeDevice* d, uint8_t bus, uint8_t dev);
void Piix3Register(Bus* io, Piix3BridgeDevice* d);
void Piix3SetResetSink(Piix3BridgeDevice* d, void (*request_reset)(void* ctx), void* ctx);

#endif
