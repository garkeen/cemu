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
// The PM1a block follows ACPI: +0x00 status (W1C), +0x02 enable, +0x04 control,
// +0x08 the free-running PM timer. The enabled fixed event raises the SCI on
// IRQ9 when control's SCI_EN is set, and SLP_EN with SLP_TYP=5 (S5) asks the
// board to end the session — the two paths a guest exercises when it reads the
// event block and shuts the machine down. S3/S4 have no wake path on this
// machine and are not entered (简化登记 D19).
typedef struct Piix4PmDevice {
  PciDevice pci;
  Bus* io;            // the I/O address space the block gets decoded in
  uint16_t base;      // the address PMBASE currently points at (0 = not programmed)
  uint8_t pmregmisc;  // bit 0 enables the block (seabios's "enable PM io space")
  uint16_t pm1_sts;   // PM1_STS: fixed-event status, write-one-to-clear
  uint16_t pm1_en;    // PM1_EN: which fixed events raise the SCI
  uint16_t pm1_cnt;   // PM1_CNT: SCI_EN, GBL_RLS and the sleep-type fields
  uint32_t last_timer;  // last 32-bit PM-timer sample, for the overflow event
  // IRQ9 (the SCI) and the board's shutdown request; both are wiring, and a
  // reset leaves them installed.
  void (*set_sci)(void* ctx, int line, int level);
  void* sci_ctx;
  void (*request_shutdown)(void* ctx);
  void* shutdown_ctx;
} Piix4PmDevice;

void Piix4PmInit(Piix4PmDevice* d, uint8_t bus, uint8_t dev);
void Piix4PmSetIoBus(Piix4PmDevice* d, Bus* io);
void Piix4PmSetIrqSink(Piix4PmDevice* d, void (*set_sci)(void* ctx, int line, int level),
                       void* ctx);
void Piix4PmSetShutdownSink(Piix4PmDevice* d, void (*request_shutdown)(void* ctx), void* ctx);
void Piix4PmPoll(Piix4PmDevice* d);
void Piix4PmReset(Piix4PmDevice* d);

#endif
