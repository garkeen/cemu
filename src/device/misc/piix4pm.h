#ifndef CEMU_DEVICE_MISC_PIIX4PM_H
#define CEMU_DEVICE_MISC_PIIX4PM_H

#include <stdint.h>

#include "bus/bus.h"
#include "device/misc/pci.h"

// Intel 82371AB (PIIX4) power management function: device 1 function 3 on bus 0,
// 8086:7113 (seabios src/hw/pci_ids.h PCI_DEVICE_ID_INTEL_82371AB_3), class
// 0x0680 (bridge, other), revision 3 — the same function QEMU's -machine pc
// exposes (`info pci`: "Bridge: PCI device 8086:7113", IRQ 9 pin A).
//
// It owns no standard BAR: the PM register block sits at an address given by the
// vendor-specific I/O base register at configuration offset 0x40 (PIIX_PMBASE),
// which firmware programs (seabios src/fw/pciinit.c piix4_pm_config_setup:
// `pci_config_writel(bdf, PIIX_PMBASE, base | 1)` then
// `pci_config_writeb(bdf, PIIX_PMREGMISC, 0x01)` to turn the block on), so
// `info pci` shows no BAR for it and the block's address is only known once
// firmware has written that register. The block is 32 bytes and SeaBIOS's own
// offsets fix what lives where: +00 PM1a event/status, +04 PM1a control,
// +08 the power management timer (`pmtimer_setup(acpi_pm_base + 0x08)`).
//
// The timer is what this machine was missing: it is a 32-bit free-running
// counter ticking at 3.579545 MHz (ACPI's defined PM timer rate, the 32.768 kHz
// watch crystal times 109.2 = 3.579545 MHz), and SeaBIOS switches its whole
// microsecond time base onto it when the function answers
// (`src/hw/timer.c pmtimer_setup`: "Using pmtimer, ioport 0x%x"). Without the
// function that message never appears and the firmware times off the 8254
// instead — a *different clock source and a different resolution*, which is why
// every firmware-side wait (the floppy timeouts, the keyboard and serial waits,
// INT 15h AH=86h, the timer list it walks while it waits) runs at a different
// scale from the model being compared against.
//
// Only the timer drives hardware. The PM1a registers read as no-event and their
// writes are stored: no SCI is delivered and the power-off path is not
// implemented — see 简化登记 D19.
typedef struct Piix4PmDevice {
  PciDevice pci;
  Bus* io;            // the I/O address space the block gets decoded in
  uint16_t base;      // the address PMBASE currently points at (0 = not programmed)
  uint8_t pmregmisc;  // bit 0 enables the block (seabios's "enable PM io space")
} Piix4PmDevice;

void Piix4PmInit(Piix4PmDevice* d, uint8_t bus, uint8_t dev);
void Piix4PmSetIoBus(Piix4PmDevice* d, Bus* io);
void Piix4PmReset(Piix4PmDevice* d);

#endif
