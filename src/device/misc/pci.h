#ifndef CEMU_DEVICE_MISC_PCI_H
#define CEMU_DEVICE_MISC_PCI_H

#include <stdint.h>

#include "bus/bus.h"

// The PCI configuration space the PC reaches through the legacy mechanism:
// an address register at 0xCF8 (bit 31 enables a transaction; bus[23:16],
// device[15:11], function[10:8], register[7:0]) and a data window at
// 0xCFC-0xCFF (Intel 440FX datasheet §3.2; QEMU hw/pci/pci.c and
// hw/pci-host/i440fx.c implement the same pair). Devices answer their 256-byte
// configuration space; an address no device claims reads all ones, which is
// how firmware enumerates the bus (seabios src/fw/pciinit.c).
//
// A device registers itself with its identity and configuration image; this
// transport owns the address register and the byte assembly, so a chipset
// model only fills in what its own registers mean.
typedef struct PciDevice {
  uint8_t bus, dev, func;
  uint8_t config[256];
  // Called after a write lands in `config`, with the byte offset touched (a
  // device whose registers drive hardware — the host bridge's PAM bytes turn
  // the BIOS ROM area into RAM — reacts here).
  void (*written)(void* ctx, uint8_t off, uint8_t val);
  void* ctx;
} PciDevice;

typedef struct PciBus {
  uint32_t addr;  // the 0xCF8 register image
  PciDevice* devices[16];
  int count;
} PciBus;

void PciInit(PciBus* b);
void PciRegister(Bus* io, PciBus* b);
void PciAddDevice(PciBus* b, PciDevice* d);

#endif
