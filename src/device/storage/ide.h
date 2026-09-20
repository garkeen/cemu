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
// all write the sector count and the LBA registers and only then the device/head
// register, so a per-drive register file would hand the command the other bay's
// stale address. Only the status and error registers are per device — they are
// how a bay reports its own last command, and an empty bay answers status 0
// forever, which is how firmware and xv6 tell "no drive" from "drive that has
// not been selected yet".
//
// Two media kinds share the controller:
//
//  - A disk answers the PIO half of ATA-4: IDENTIFY DEVICE (0xEC), READ SECTORS
//    (0x20), WRITE SECTORS (0x30) and FLUSH CACHE (0xE7), in LBA-28 or CHS
//    addressing, one 512-byte sector per DRQ handshake (ATA/ATAPI-7 §6.3 PIO
//    data transfer).
//  - A CD-ROM answers the packet (ATAPI) command set: IDENTIFY PACKET DEVICE
//    (0xA1) and PACKET (0xA0), whose 12-byte command descriptor block arrives
//    through the data port and carries a SCSI-style command (§9.5/§9.6). Its
//    logical blocks are 2048 bytes. After a reset a packet device presents the
//    ATAPI signature (sector count 1, sector number 1, cylinder low 0x14,
//    cylinder high 0xEB) — that is how a driver recognises the bay before it
//    has read IDENTIFY PACKET DEVICE.
//
// Buses and drivers do not implement a command by answering nothing: an
// unimplemented ATA command is aborted (ABRT), an unimplemented CDB reports a
// sense key. Bus-master DMA (the BAR4 engine) is not modelled; see 简化登记
// D19.
enum { kIdeDrivesPerChannel = 2 };

// Logical block sizes. The ATA register command set moves 512-byte sectors; the
// packet command set moves the CD-ROM's 2048-byte blocks (§9.6, MMC).
enum {
  kAtaBlockSize = 512,
  kAtapiBlockSize = 2048,
  kMaxBlockSize = kAtapiBlockSize,
  kAtapiCdbSize = 12,  // 12-byte packets (IDENTIFY PACKET DEVICE word 0 bit 5 = 0)
};

// Media kinds for IdeAttach.
enum { kIdeMediaDisk = 0, kIdeMediaCd = 1 };

// One bay: the medium, its geometry, and the two registers the host reads back
// from that particular device.
typedef struct IdeDrive {
  HostFile* image;  // the medium; NULL = empty bay
  int atapi;        // packet device: 2048-byte blocks and the CDB command set
  // Medium size in logical blocks (512-byte for a disk, 2048-byte for a CD):
  // what IDENTIFY words 60/61 and the READ CAPACITY reply report.
  int64_t blocks;
  // CHS geometry, reported by IDENTIFY words 1/3/6 and used to translate CHS
  // commands. 16 heads / 63 sectors per track is the LBA-assist translation
  // firmware and QEMU's hd_geometry_guess use for a drive without a real one. A
  // CD has no geometry to report (CHS addressing is LBA on a packet device).
  uint16_t cylinders;
  uint8_t heads;
  uint8_t sectors_per_track;
  uint8_t error;   // error register (§7.10.2); for a packet device the sense
                   // key sits in its high nibble
  uint8_t status;  // status register (§7.10.9); zero while the bay is empty
  // The sense key and additional sense code of the last failed packet command,
  // which REQUEST SENSE hands back (§10.3.2).
  uint8_t sense_key;
  uint8_t asc;
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
  // was written. Data moves in DRQ rounds: a round hands over as much as one
  // logical block holds, the ATAPI byte count limit, and the bytes the command
  // has left, whichever is smallest (§6.3, §9.6).
  int active;      // drive executing, -1 = idle
  int is_write;    // direction of the active transfer
  int packet;      // what is moving is the CDB itself, not data
  int64_t lba;     // logical block being transferred
  int64_t left;    // bytes left in the command's data phase
  int round;       // bytes of the current DRQ round not yet moved
  int used;        // bytes of the current block already moved
  int block_size;  // bytes per logical block for the active command
  int limit;       // per-round byte limit: one block for ATA, the packet
                   // device's byte count limit register value for ATAPI
  int refill;      // the buffer is reloaded at every block boundary — a medium
                   // transfer, as opposed to a reply that already sits in it
  int irq_pending;  // a completion to report (masked by devctrl nIEN)
  uint8_t buf[kMaxBlockSize];
} IdeChannel;

struct IdeDevice {
  PciDevice pci;
  IdeChannel channels[2];  // [0] = primary (IRQ14), [1] = secondary (IRQ15)
  void (*set_irq)(void* ctx, int line, int level);
  void* irq_ctx;
};

void IdeInit(IdeDevice* d, uint8_t bus, uint8_t dev);
// Attaches a medium to one bay: channel 0/1, drive 0 (master) / 1 (slave).
// Returns 0 on success; the image must be a whole number of logical blocks
// (512 bytes for kIdeMediaDisk, 2048 for kIdeMediaCd).
int IdeAttach(IdeDevice* d, int channel, int drive, const char* path, int media);
// The four legacy port windows on the I/O bus.
void IdeRegister(Bus* io, IdeDevice* d);
void IdeSetIrqSink(IdeDevice* d, void (*set_irq)(void* ctx, int line, int level), void* ctx);
// Closes the attached images.
void IdeDestroy(IdeDevice* d);

#endif
