#ifndef CEMU_DEVICE_STORAGE_IDE_H
#define CEMU_DEVICE_STORAGE_IDE_H

#include <stdint.h>

#include "bus/bus.h"
#include "device/misc/pci.h"
#include "host/host.h"

// Intel 82371SB (PIIX3) IDE controller: the PC's parallel ATA host adapter,
// two channels of two drives. Guests reach it through the legacy task file —
// an 8-byte command block per channel (0x1F0 / 0x170) plus a control block
// (0x3F6 / 0x376) — while PCI configuration space identifies it as 8086:7010,
// class 0x0101 with programming interface 0x80. Those two bytes are the
// contract with firmware: SeaBIOS (src/hw/ata.c init_pciata) hands each channel
// its ISA-mode ports and the fixed IRQ 14/15 exactly when the channel is not
// switched to native mode, and the device ID routes it through piix_ide_setup,
// which only pokes the channel-enable bits at 0x40/0x42. QEMU's hw/ide/piix.c
// carries the same identity (8086:7010, class 0x010180, BAR0..3 =
// 0x1F1/0x3F5/0x171/0x375).
//
// The task file belongs to the CHANNEL, not to a drive (ATA/ATAPI-7 §7.10: the
// host interface is one register file, and the drive/head register's bit 4
// picks which of the two devices executes the next command). Every driver
// depends on that: xv6's idestart, libata's ata_tf_load and SeaBIOS's send_cmd
// all write the sector count and the LBA registers *first* and the device/head
// register last, so a per-drive register file would hand the command the other
// bay's stale address. Only the status and error registers are per device —
// they are how a bay reports its own last command, and an empty bay answers
// status 0 forever, which is how firmware and xv6 tell "no drive" from "drive
// that has not been selected yet".
//
// The drive is the PIO half of ATA-4: IDENTIFY DEVICE (0xEC), READ SECTORS
// (0x20), WRITE SECTORS (0x30) and FLUSH CACHE (0xE7), in LBA-28 or CHS
// addressing, one 512-byte sector per DRQ handshake (ATA/ATAPI-7 §6.3 PIO data
// transfer). Everything else reports ABRT, which is what a drive that does not
// implement a command answers — including IDENTIFY PACKET DEVICE (0xA1), the
// probe SeaBIOS uses to tell an ATAPI drive from an ATA one. Bus-master DMA
// (the BAR4 engine) is not modelled; see 简化登记 D19.
enum { kIdeDrivesPerChannel = 2 };

// One bay: the medium, its geometry, and the two registers the host reads back
// from that particular device.
typedef struct IdeDrive {
  HostFile* image;  // the medium; NULL = empty bay
  int64_t sectors;  // medium size in 512-byte sectors
  // CHS geometry, reported by IDENTIFY words 1/3/6 and used to translate CHS
  // commands. 16 heads / 63 sectors per track is the LBA-assist translation
  // firmware and QEMU's hd_geometry_guess use for a drive without a real one.
  uint16_t cylinders;
  uint8_t heads;
  uint8_t sectors_per_track;
  uint8_t error;   // error register (§7.10.2)
  uint8_t status;  // status register (§7.10.9); zero while the bay is empty
} IdeDrive;

typedef struct IdeDevice IdeDevice;

typedef struct IdeChannel {
  IdeDevice* dev;      // for the interrupt sink (the board wires it)
  uint16_t cmd_base;   // 0x1F0 / 0x170: the eight task file registers
  uint16_t ctrl_base;  // 0x3F6 / 0x376: alternate status and device control
  int irq_line;        // 14 / 15, the channel's fixed ISA interrupt
  IdeDrive drives[kIdeDrivesPerChannel];  // [0] = master, [1] = slave
  // The channel's task file, as the host wrote it: sector count, address and
  // the device/head register that selects the bay.
  uint8_t count;
  uint8_t lba_low;
  uint8_t lba_mid;
  uint8_t lba_high;
  uint8_t head;
  int selected;     // the bay the device/head register picked
  uint8_t devctrl;  // control block: nIEN (bit 1), SRST (bit 2), HD15 (bit 3)
  // An in-flight PIO transfer. One command runs per channel at a time; the
  // active drive is the one the device/head register selected when the command
  // was written.
  int active;     // drive executing, -1 = idle
  int is_write;   // direction of the active transfer
  int64_t lba;    // sector being transferred
  int remaining;  // sectors left in the command
  int done;       // bytes of the current sector already moved
  int irq_pending;  // a completion to report (masked by devctrl nIEN)
  uint8_t buf[512];
} IdeChannel;

struct IdeDevice {
  PciDevice pci;
  IdeChannel channels[2];  // [0] = primary (IRQ14), [1] = secondary (IRQ15)
  void (*set_irq)(void* ctx, int line, int level);
  void* irq_ctx;
};

void IdeInit(IdeDevice* d, uint8_t bus, uint8_t dev);
// Attaches a disk image to one bay: channel 0/1, drive 0 (master) / 1 (slave).
// Returns 0 on success; the image must be a whole number of sectors.
int IdeAttach(IdeDevice* d, int channel, int drive, const char* path);
// The four legacy port windows on the I/O bus.
void IdeRegister(Bus* io, IdeDevice* d);
void IdeSetIrqSink(IdeDevice* d, void (*set_irq)(void* ctx, int line, int level), void* ctx);
// Closes the attached images.
void IdeDestroy(IdeDevice* d);

#endif
