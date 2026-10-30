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
//
// SMBus host controller (PIIX4 datasheet §5 "SMBus"; QEMU hw/i2c/pm_smbus.c
// carries the same register file inside this PCI function, hw/i2c/core.c the
// bus, hw/i2c/smbus_slave.c the device side of the protocol).
//
// The block is a 64-byte window whose address lives in configuration offset
// 0x90 (SMB_BASE, bit 0 = I/O-space indicator) and whose decode hangs off bit 0
// of offset 0xD2. QEMU's pc machine leaves the base at 0xb100 with the gate
// open (hw/i386/pc_piix.c passes smb_io_base=0xb100, hw/acpi/piix4.c writes
// 0xb101 and 0xD2=0x09 at realize), so a guest finds the controller at 0xb100
// before any firmware touches it.
//
// The interrupt this controller could raise reaches no pin on this machine
// (QEMU leaves PMSMBus.set_irq NULL), so HST_CNT's INTREN decides only *when*
// the transaction runs, never an IRQ line: with INTREN clear the transaction
// waits for the guest's HST_STS read (the AMIBIOS workaround hw/i2c/pm_smbus.c
// documents).
enum {
  // The window's size (piix4.c's memory_region_init_io(..., 64)) and the
  // address field of SMB_BASE: bits 15:6.
  kSmbusBlockSize = 64,
  kSmbusBaseMask = 0xffc0,
  // PM_SMBUS_MAX_MSG_SIZE (pm_smbus.h) and SMBUS_DATA_MAX_LEN (smbus_slave.h:
  // command + count + 32 bytes of data).
  kSmbusMaxMsgSize = 32,
  kSmbusDataMax = 34,
  // The devices QEMU's pc machine puts on this bus: eight SPD EEPROMs at
  // 0x50-0x57 (hw/i386/pc_piix.c smbus_eeprom_init(pcms->smbus, 8, NULL, 0)).
  kSmbusSlaveCount = 8,
};

// One device on the I2C bus: QEMU's SMBusDevice, whose class supplies these two
// hooks and whose base class carries the protocol state (the mode and the write
// buffer) a transfer leaves behind (hw/i2c/smbus_slave.c).
typedef struct SmbusSlave {
  uint8_t address;  // 7-bit I2C address
  uint8_t (*ReceiveByte)(void* ctx);
  void (*WriteData)(void* ctx, const uint8_t* buf, int len);
  void* ctx;
  int mode;                         // this device's protocol state (piix4pm.c)
  uint8_t write_buf[kSmbusDataMax];  // bytes the write phase collected
  int write_len;
} SmbusSlave;

typedef struct Piix4Smbus {
  uint16_t base;  // where SMB_BASE decodes (0 = the window is off)
  uint8_t sts;    // HST_STS: HOST_BUSY, INTR, DEV_ERR, BUS_ERR, FAILED, BYTE_DONE
  uint8_t ctl;    // HST_CNT: INTREN, KILL, LAST_BYTE, START, protocol bits 4:2
  uint8_t cmd;    // HST_CMD
  uint8_t addr;   // HST_ADD: 7-bit address in bits 7:1, direction in bit 0
  uint8_t data0;  // HST_DAT0
  uint8_t data1;  // HST_DAT1
  uint8_t data[kSmbusMaxMsgSize];  // the block buffer HST_BLKDAT walks
  uint8_t blkdata;                 // HST_BLKDAT's single-byte window
  uint8_t auxctl;                  // HST_AUXCTL: PEC_EN, BLK (32-byte block mode)
  uint32_t index;                  // position inside the block buffer
  int op_done;                     // the byte-by-byte transfer has finished
  int in_i2c_block_read;
  int start_on_status_read;
  SmbusSlave* slaves[kSmbusSlaveCount];
  int slave_count;
  // The devices the current transfer selected, and whether its address was the
  // broadcast one (hw/i2c/core.c's current_devs/broadcast).
  SmbusSlave* current[kSmbusSlaveCount];
  int current_count;
  int broadcast;
} Piix4Smbus;

// Intel 82371AB (PIIX4) power management function: device 1 function 3 on bus 0,
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
  Piix4Smbus smb;  // the SMBus host controller and the devices on its bus
} Piix4PmDevice;

void Piix4PmInit(Piix4PmDevice* d, uint8_t bus, uint8_t dev);
void Piix4PmSetIoBus(Piix4PmDevice* d, Bus* io);
void Piix4PmSetIrqSink(Piix4PmDevice* d, void (*set_sci)(void* ctx, int line, int level),
                       void* ctx);
void Piix4PmSetShutdownSink(Piix4PmDevice* d, void (*request_shutdown)(void* ctx), void* ctx);
void Piix4PmPoll(Piix4PmDevice* d);
void Piix4PmReset(Piix4PmDevice* d);

// Puts a device on the SMBus. A reset leaves the bus populated: a device is
// soldered onto the board, not programmed into a register.
void Piix4SmbusAttach(Piix4PmDevice* d, SmbusSlave* slave);
#endif
