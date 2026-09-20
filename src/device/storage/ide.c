#include "device/storage/ide.h"

#include <string.h>

// Legacy port map. In compatibility mode — which is what the programming
// interface byte 0x80 without bits 0/2 means — the channels answer at the ISA
// addresses, and that is what the BARs mirror (seabios src/fw/pciinit.c
// PORT_ATA1_CMD_BASE 0x1F0 / PORT_ATA1_CTRL_BASE 0x3F6, PORT_ATA2_* 0x170 /
// 0x376; Intel 82371SB datasheet §3.2 "IDE Compatibility mode").
enum {
  kPrimaryCmdBase = 0x1f0,
  kPrimaryCtrlBase = 0x3f6,
  kSecondaryCmdBase = 0x170,
  kSecondaryCtrlBase = 0x376,
  kCmdBlockSize = 8,
  // Only the device-control / alternate-status byte is decoded; the byte above
  // it (the AT drive address register) is not an ATA register — see 简化登记
  // D19.
  kCtrlBlockSize = 1,
  kPrimaryIrq = 14,
  kSecondaryIrq = 15,
};

// Task file register offsets from the command block base (ATA/ATAPI-7 §7.10):
// data, error/features, sector count, LBA low/mid/high (cylinder low/high,
// sector number), drive/head, status/command.
enum {
  kRegData = 0,
  kRegFeatureError = 1,
  kRegSectorCount = 2,
  kRegLbaLow = 3,
  kRegLbaMid = 4,
  kRegLbaHigh = 5,
  kRegDriveHead = 6,
  kRegCommandStatus = 7,
};

// Status register bits (§7.10.9): ERR, DRQ, DSC (seek complete), DF (device
// fault), DRDY, BSY.
enum {
  kStatusErr = 0x01,
  kStatusDrq = 0x08,
  kStatusDsc = 0x10,
  kStatusDf = 0x20,
  kStatusDrdy = 0x40,
  kStatusBsy = 0x80,
};
// Error register bits (§7.10.2): ABRT (bit 2) is the only one we set. For a
// packet device the error register carries the sense key in bits 7:4 while ERR
// is asserted (§9.6 "error register").
enum { kErrorAbrt = 0x04 };
// Resting status of a drive that will accept a command: ready + seek complete
// (QEMU's ide_bus_reset leaves the same READY|SEEK pair).
enum {
  kStatusReady = kStatusDrdy | kStatusDsc,
  kStatusAborted = kStatusDrdy | kStatusErr,
};
// Device control register bits (§7.10.10 device control).
enum { kDevCtrlNien = 0x02, kDevCtrlSrst = 0x04, kDevCtrlHd15 = 0x08 };

// The ATA command set a disk implements (ATA-4, LBA-28 PIO). Every other
// command — including the LBA-48 extended commands and the packet commands —
// is aborted, which is how a drive answers a command it does not implement
// (§6.3 command protocol).
enum {
  kCmdReadSectors = 0x20,
  kCmdWriteSectors = 0x30,
  kCmdFlushCache = 0xe7,
  kCmdIdentifyDevice = 0xec,
};

// The packet (ATAPI) command set a CD-ROM implements (§9.5 packet command
// protocol, §7.10.2 for the IDENTIFY data): PACKET carries a 12-byte command
// descriptor block through the data port, IDENTIFY PACKET DEVICE is the probe
// that tells a packet device from a register-set one (SeaBIOS's ata_detect asks
// for the packet identity first and only then for IDENTIFY DEVICE).
enum {
  kCmdPacket = 0xa0,
  kCmdIdentifyPacket = 0xa1,
};

// The CDBs answered: the CD-ROM subset firmware's boot path and an OS's
// ATA/ATAPI driver ask for — readiness and sense (SPC), identity (SPC), capacity
// and reads (MMC).
enum {
  kCdbTestUnitReady = 0x00,
  kCdbRequestSense = 0x03,
  kCdbInquiry = 0x12,
  kCdbStartStopUnit = 0x1b,
  kCdbReadCapacity = 0x25,
  kCdbRead10 = 0x28,
  kCdbRead12 = 0xa8,
};
// Sense keys and additional sense codes (§10.3.2, SPC sense data).
enum {
  kSenseNoSense = 0x0,
  kSenseIllegalRequest = 0x5,
  kAscLogicalBlockOutOfRange = 0x21,
  kAscInvalidFieldInCdb = 0x24,
};
// Interrupt reason: the byte a packet device shows in the sector count register
// (§9.6 table "interrupt reason"): command/data, and the direction. Status and
// command packets read back as "command complete".
enum {
  kAtapiReasonCd = 0x01,  // C/D: what the host reads is a command packet
  kAtapiReasonIo = 0x02,  // I/O: the host is taking data out of the device
};
// Bytes of the ATAPI signature a packet device presents after a reset (§9.5):
// sector count 1, sector number 1, cylinder low/high 0x14EB.
enum {
  kSignatureCount = 1,
  kSignatureNumber = 1,
  kSignatureCylLow = 0x14,
  kSignatureCylHigh = 0xeb,
};
enum { kIdentifyWords = 256 };
// INQUIRY answers 36 bytes: the header plus the vendor/product/revision strings
// (SPC INQUIRY data format).
enum { kInquiryBytes = 36 };

// PCI configuration space (PCI spec §6.1/§6.2) and this function's identity.
enum {
  kCfgVendor = 0x00,
  kCfgDevice = 0x02,
  kCfgRevision = 0x08,
  kCfgProgIf = 0x09,
  kCfgSubClass = 0x0a,
  kCfgClass = 0x0b,
  kCfgHeaderType = 0x0e,
  kCfgBar0 = 0x10,
  kCfgBar1 = 0x14,
  kCfgBar2 = 0x18,
  kCfgBar3 = 0x1c,
  kCfgBar4 = 0x20,
  kCfgIntLine = 0x3c,
  kCfgIntPin = 0x3d,
};
enum {
  kVendorIntel = 0x8086,
  kDevicePiix3Ide = 0x7010,  // PCI_DEVICE_ID_INTEL_82371SB_1 (seabios pci_ids.h)
  kRevision = 0,
  // Bus master capable (bit 7), both channels *not* in native mode: bits 0 and
  // 2 clear put the channels on the fixed ISA ports and IRQs (PCI spec §6.2.5
  // "programming interface"; seabios init_pciata reads exactly these bits).
  kProgIf = 0x80,
  kSubClassIde = 0x01,
  kClassStorage = 0x01,
  kBarPrimaryCmd = kPrimaryCmdBase | 1,    // 8 ports, I/O space indicator set
  kBarPrimaryCtrl = kPrimaryCtrlBase | 1,  // 2 ports
  kBarSecondaryCmd = kSecondaryCmdBase | 1,
  kBarSecondaryCtrl = kSecondaryCtrlBase | 1,
  kBarPrimaryCmdSize = 8,
  kBarPrimaryCtrlSize = 2,
  kBarSecondaryCmdSize = 8,
  kBarSecondaryCtrlSize = 2,
  kIntLine = kPrimaryIrq,
};

static void Put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)(v >> 8);
}

static void Put32(uint8_t* p, uint32_t v) {
  Put16(p, (uint16_t)(v & 0xffff));
  Put16(p + 2, (uint16_t)(v >> 16));
}

// CDB fields and the replies that carry numbers are big-endian (SCSI), while
// the ATA task file is little-endian.
static void Put32Be(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static IdeDrive* Selected(IdeChannel* ch) { return &ch->drives[ch->selected]; }

// The channel's interrupt line. A request stays latched until the host reads
// the status register (that read *is* the drive's interrupt acknowledge), and
// nIEN in the device control register holds the line down while it is set
// (device control register §7.10.10; QEMU hw/ide/core.c ide_set_irq).
static void UpdateIrq(IdeChannel* ch) {
  int level = (ch->irq_pending && !(ch->devctrl & kDevCtrlNien)) ? 1 : 0;
  if (ch->dev->set_irq) ch->dev->set_irq(ch->dev->irq_ctx, ch->irq_line, level);
}

static void RaiseIrq(IdeChannel* ch) {
  ch->irq_pending = 1;
  UpdateIrq(ch);
}

static void ClearIrq(IdeChannel* ch) {
  ch->irq_pending = 0;
  UpdateIrq(ch);
}

// A disk answers a command it cannot execute by aborting it: ABRT in the error
// register, ERR in the status register, and the interrupt that ends the command
// (§6.3; the status register keeps DRDY set so the host can tell the drive is
// still there).
static void Abort(IdeChannel* ch, IdeDrive* dr) {
  dr->error = kErrorAbrt;
  dr->status = kStatusAborted;
  ch->active = -1;
  ch->left = 0;
  ch->round = 0;
  ch->packet = 0;
  RaiseIrq(ch);
}

// A packet command the device cannot run: the error register carries the sense
// key in its high nibble, status has ERR, and the key plus the additional sense
// code wait for REQUEST SENSE (§9.6, §10.3.2).
static void AtapiError(IdeChannel* ch, int sense_key, int asc) {
  IdeDrive* dr = &ch->drives[ch->active];
  dr->error = (uint8_t)(sense_key << 4);
  dr->status = kStatusDrdy | kStatusErr;
  dr->sense_key = (uint8_t)sense_key;
  dr->asc = (uint8_t)asc;
  ch->count = (uint8_t)(kAtapiReasonCd | kAtapiReasonIo);
  ch->active = -1;
  ch->left = 0;
  ch->round = 0;
  ch->packet = 0;
  RaiseIrq(ch);
}

// A packet command with no data phase completes with the status and interrupt
// reason a finished transfer leaves behind (§9.6).
static void CmdOk(IdeChannel* ch) {
  IdeDrive* dr = &ch->drives[ch->active];
  dr->error = 0;
  dr->status = kStatusReady;
  ch->count = (uint8_t)(kAtapiReasonCd | kAtapiReasonIo);
  ch->active = -1;
  ch->left = 0;
  ch->round = 0;
  ch->packet = 0;
  RaiseIrq(ch);
}

static void ExecPacket(IdeChannel* ch);  // defined below: the CDB dispatcher

// The medium could not be read where the command asked for it. A disk aborts
// the command; a packet device reports the block as out of range, which is the
// sense the reference implementations answer a bad CD read with (QEMU hw/ide/
// core.c ide_atapi_io_error).
static void MediumReadFailed(IdeChannel* ch) {
  IdeDrive* dr = &ch->drives[ch->active];
  if (dr->atapi)
    AtapiError(ch, kSenseIllegalRequest, kAscLogicalBlockOutOfRange);
  else
    Abort(ch, dr);
}

// Selecting a packet bay presents the ATAPI signature. An ATA bay leaves the
// task file exactly as the host wrote it, because drivers program the address
// registers *before* the device/head register (xv6's idestart writes the sector
// count and the LBA, then selects) — clearing them here would send the command
// to the wrong sector.
static void PresentSignature(IdeChannel* ch) {
  if (!ch->drives[ch->selected].atapi) return;
  ch->count = kSignatureCount;
  ch->lba_low = kSignatureNumber;
  ch->lba_mid = kSignatureCylLow;
  ch->lba_high = kSignatureCylHigh;
}

// Software reset (SRST): the channel's task file returns to its power-on state,
// the devices report ready again, and any transfer in flight is dropped (§8.3
// software reset protocol).
static void ResetChannel(IdeChannel* ch) {
  ch->count = 0;
  ch->lba_low = 0;
  ch->lba_mid = 0;
  ch->lba_high = 0;
  ch->head = 0;
  for (int i = 0; i < kIdeDrivesPerChannel; i++) {
    IdeDrive* dr = &ch->drives[i];
    dr->error = 0;
    dr->sense_key = kSenseNoSense;
    dr->asc = 0;
    // An empty bay keeps answering zero: that is how firmware and xv6 tell a
    // bay with no drive from one with a drive that has not been selected yet.
    dr->status = dr->image ? kStatusReady : 0;
  }
  ch->selected = 0;
  ch->active = -1;
  ch->is_write = 0;
  ch->packet = 0;
  ch->left = 0;
  ch->round = 0;
  ch->used = 0;
  ch->refill = 0;
  ch->block_size = kAtaBlockSize;
  ch->limit = kAtaBlockSize;
  PresentSignature(ch);
  ClearIrq(ch);
}

// ATA strings put the first character of a pair in the *high* byte of the word
// (§7.10.2 words 10..19, 23..26, 27..46), so in little-endian memory the two
// characters of a word read back swapped. SeaBIOS undoes exactly that swap
// (src/hw/ata.c ata_extract_model byte-swaps every word). Fields are padded
// with spaces, as the standard specifies.
static void PutAtaString(uint16_t* id, int first_word, int chars, const char* s) {
  int len = (int)strlen(s);
  for (int i = 0; i < chars; i += 2) {
    char a = i < len ? s[i] : ' ';
    char b = i + 1 < len ? s[i + 1] : ' ';
    id[first_word + i / 2] = (uint16_t)(((uint16_t)(uint8_t)a << 8) | (uint8_t)b);
  }
}

// IDENTIFY DEVICE data (§7.10.2). Words left zero are the ones this drive
// answers "not supported" to: 47/59 (R/W multiple), 63/88 and 64..70 (DMA modes
// and their timing), 83 and 100..103 (LBA-48 capacity), 93 bits other than the
// ones below.
static void BuildIdentify(const IdeChannel* ch, int drive, uint16_t* id) {
  const IdeDrive* dr = &ch->drives[drive];
  memset(id, 0, kAtaBlockSize);
  // Word 0: bit 6 = fixed (non-removable) device, bit 7 clear = ATA, not ATAPI.
  id[0] = 0x0040;
  id[1] = dr->cylinders;
  id[3] = dr->heads;
  id[6] = dr->sectors_per_track;
  PutAtaString(id, 10, 20, "CEMU0000000000000001");  // serial number
  PutAtaString(id, 23, 8, "1.0");                    // firmware revision
  PutAtaString(id, 27, 40, "CEMU VIRTUAL DISK");     // model number
  // Word 49: capabilities — bit 9 = LBA supported.
  id[49] = 0x0200;
  // Word 53: bit 0 = words 54..58 carry data.
  id[53] = 0x0001;
  id[54] = dr->cylinders;
  id[55] = dr->heads;
  id[56] = dr->sectors_per_track;
  uint32_t chs_sectors = (uint32_t)dr->cylinders * dr->heads * dr->sectors_per_track;
  id[57] = (uint16_t)(chs_sectors & 0xffff);  // current capacity, words 57..58
  id[58] = (uint16_t)(chs_sectors >> 16);
  id[60] = (uint16_t)(dr->blocks & 0xffff);  // LBA-28 capacity, words 60..61
  id[61] = (uint16_t)((dr->blocks >> 16) & 0xffff);
  // Word 80: the ATA revision this command set is — bit 4 = ATA-4.
  id[80] = 0x0008;
  // Word 93: hardware reset result. Bit 14 = device 0 detected, bit 13 = device
  // 1 detected, bit 0 always set. Bit 6 is the cable ID pin, which this board
  // does not wire, so it reads 0 — and that is also what keeps firmware
  // probing the slave bay: SeaBIOS reads (word 93 & 0xdf61) == 0x4041 as
  // "device 0 answered device 1's selects, so there is no device 1" and stops
  // (src/hw/ata.c ata_detect).
  id[93] = (uint16_t)(0x4001 | (ch->drives[drive ^ 1].image ? 0x2000 : 0));
}

// IDENTIFY PACKET DEVICE data (§7.10.2). Word 0 is where a packet device says
// what it is: bits 15:14 = 10b (a packet device), bits 12:8 = the device type
// (5 = CD-ROM), bit 7 = removable, bit 5 clear = 12-byte command packets.
// SeaBIOS reads exactly those fields to decide it has found a bootable CD
// (src/hw/ata.c init_drive_atapi: iscd = (word 0 >> 8) & 0x1f == 5).
static void BuildIdentifyPacket(uint16_t* id) {
  memset(id, 0, kAtaBlockSize);
  id[0] = 0x8580;
  // Word 20: buffer type. Word 21: cache size in 512-byte sectors. Word 22:
  // ECC bytes — the values the reference implementation reports for a CD-ROM
  // (QEMU hw/ide/core.c ide_atapi_identify).
  id[20] = 3;
  id[21] = 512;
  id[22] = 4;
  PutAtaString(id, 23, 8, "1.0");                // firmware revision
  PutAtaString(id, 27, 40, "CEMU VIRTUAL CD-ROM");  // model number
  // Word 49: capabilities — bit 9 = LBA supported, bit 8 = no DMA (bus-master
  // DMA is not modelled; see 简化登记 D19).
  id[49] = 0x0200;
  // Word 53: bit 0 = words 54..58 carry data, bit 1 = words 64..70 carry data.
  id[53] = 0x0003;
  // Word 63: the one PIO mode this device offers (mode 0, §7.10.2 word 63 bits
  // 7:0); words 64/67/68 give its timing. No multiword DMA mode is advertised
  // (bits 15:8 stay clear) because the BAR4 engine does not exist.
  id[63] = 0x0001;
  id[64] = 0x0001;  // advanced PIO modes supported: mode 0 only
  id[67] = 0x00f0;  // minimum PIO cycle without IORDY (240 ns)
  id[68] = 0x00f0;  // minimum PIO cycle with IORDY (240 ns)
  // Word 80: bit 4 = ATA-4, the revision whose packet command set this is.
  id[80] = 0x0008;
  // Word 0 of the identify data is the only place the medium size appears for a
  // packet device (there is no LBA capacity word pair): the CDB READ CAPACITY
  // is what reports it (§10.3.5).
}

// INQUIRY data (SPC): peripheral qualifier and device type, the removable bit,
// and the three identity strings a driver prints.
static void BuildInquiry(uint8_t* buf) {
  memset(buf, 0, kInquiryBytes);
  buf[0] = 0x05;  // peripheral device type 5 = CD-ROM
  buf[1] = 0x80;  // removable medium
  buf[2] = 0x00;  // no version conformance claimed
  buf[3] = 0x02;  // response data format 2
  buf[4] = kInquiryBytes - 5;  // additional length (SPC: one byte, 31 here)
  memcpy(&buf[8], "CEMU    ", 8);            // vendor identification
  memcpy(&buf[16], "VIRTUAL CD-ROM  ", 16);  // product identification
  memcpy(&buf[32], "1.0 ", 4);               // product revision level
}

// The sector a command addresses: bits 27:0 of the task file when the
// drive/head register selects LBA (bit 6), else the CHS triple translated
// through the drive's geometry (§7.10.6 drive/head register; heads/sectors per
// track from IDENTIFY words 3/6).
static int64_t CommandLba(const IdeChannel* ch, const IdeDrive* dr, int* ok) {
  *ok = 1;
  if (ch->head & 0x40) {
    return ((int64_t)(ch->head & 0x0f) << 24) | ((int64_t)ch->lba_high << 16) |
           ((int64_t)ch->lba_mid << 8) | (int64_t)ch->lba_low;
  }
  uint32_t cyl = ((uint32_t)ch->lba_high << 8) | ch->lba_mid;
  uint32_t sector = ch->lba_low;  // one-based
  uint32_t head = ch->head & 0x0f;
  if (sector == 0 || sector > dr->sectors_per_track || head >= dr->heads) {
    *ok = 0;
    return 0;
  }
  return ((int64_t)cyl * dr->heads + head) * dr->sectors_per_track + (int64_t)(sector - 1);
}

// Begins a DRQ round: a medium transfer fills the buffer when it crosses a block
// boundary, and the host is told how many bytes the round covers — one logical
// block, the ATAPI byte count limit, or what the command has left, whichever is
// smallest (§6.3 PIO data transfer; §9.6 PACKET). Returns -1 when the medium
// cannot be read there.
static int StartRound(IdeChannel* ch) {
  if (ch->refill && !ch->is_write && ch->used == 0) {
    if (HostFileReadAt(ch->drives[ch->active].image, ch->lba * ch->block_size, ch->buf,
                       (size_t)ch->block_size) != (size_t)ch->block_size)
      return -1;
  }
  int64_t round = ch->left;
  if (ch->refill) {
    int64_t block_left = ch->block_size - ch->used;
    if (round > block_left) round = block_left;
  }
  if (round > ch->limit) round = ch->limit;
  ch->round = (int)round;
  ch->drives[ch->active].status |= kStatusDrq;
  return 0;
}

// The end of a command's data phase: DRQ drops, the packet device's interrupt
// reason turns to "command complete", and the completion interrupt goes out
// (the host reads the status register to acknowledge it).
static void EndCommand(IdeChannel* ch) {
  IdeDrive* dr = &ch->drives[ch->active];
  if (dr->atapi) ch->count = (uint8_t)(kAtapiReasonCd | kAtapiReasonIo);
  dr->status &= (uint8_t)~kStatusDrq;
  ch->active = -1;
  ch->round = 0;
  RaiseIrq(ch);
}

// Moving bytes is what advances the transfer: when a round is drained the next
// one starts, or the command ends. A medium write leaves through the file block
// by block, and the command packet a PACKET command was collecting is dispatched.
static void MoveBytes(IdeChannel* ch, int size) {
  ch->round -= size;
  ch->used += size;
  ch->left -= size;
  if (ch->round > 0) return;
  if (ch->packet) {
    ch->packet = 0;
    ExecPacket(ch);
    return;
  }
  if (ch->used == ch->block_size && (ch->is_write || ch->refill)) {
    IdeDrive* dr = &ch->drives[ch->active];
    if (ch->is_write &&
        HostFileWriteAt(dr->image, ch->lba * ch->block_size, ch->buf,
                        (size_t)ch->block_size) != (size_t)ch->block_size) {
      Abort(ch, dr);
      return;
    }
    ch->used = 0;
    ch->lba++;
  }
  if (ch->left > 0) {
    if (StartRound(ch) != 0) {
      MediumReadFailed(ch);
      return;
    }
    // A packet device reports every round to the host; the ATA register command
    // set only interrupts when the whole command is done (§6.3, §9.6).
    if (ch->drives[ch->active].atapi) RaiseIrq(ch);
    return;
  }
  EndCommand(ch);
}

static uint64_t ReadData(IdeChannel* ch, int size) {
  if (ch->active < 0 || ch->is_write || ch->round < size) return 0;
  uint64_t v = 0;
  for (int i = 0; i < size; i++) v |= (uint64_t)ch->buf[ch->used + i] << (8 * i);
  MoveBytes(ch, size);
  return v;
}

static void WriteData(IdeChannel* ch, int size, uint64_t val) {
  if (ch->active < 0 || !ch->is_write || ch->round < size) return;
  for (int i = 0; i < size; i++) ch->buf[ch->used + i] = (uint8_t)(val >> (8 * i));
  MoveBytes(ch, size);
}

// A packet command's data-in phase: DRQ plus the "data to the host" interrupt
// reason. A medium read refills the buffer block by block; a reply that is
// already in the buffer just moves out. A zero-length transfer has no data
// phase at all and completes where it stands (§9.6).
static void StartDataIn(IdeChannel* ch, int64_t lba, int bytes, int refill) {
  // The data phase moves the medium's logical blocks (2048 bytes on a CD-ROM),
  // not the 12-byte packet the command phase collected: leaving block_size at
  // the CDB size would carve the transfer into 12-byte pieces read from the
  // wrong offsets (§9.6).
  ch->block_size = kAtapiBlockSize;
  ch->is_write = 0;
  ch->refill = refill;
  ch->used = 0;
  ch->lba = lba;
  ch->left = bytes;
  if (bytes == 0) {
    CmdOk(ch);
    return;
  }
  if (StartRound(ch) != 0) {
    MediumReadFailed(ch);
    return;
  }
  ch->count = kAtapiReasonIo;
  RaiseIrq(ch);
}

// The CDB a PACKET command just collected decides what the device does
// (ATA/ATAPI-7 §10 packet command set; SPC for INQUIRY and REQUEST SENSE, MMC
// for READ CAPACITY and the READ family).
static void ExecPacket(IdeChannel* ch) {
  IdeDrive* dr = &ch->drives[ch->active];
  const uint8_t* cdb = ch->buf;
  switch (cdb[0]) {
    case kCdbTestUnitReady:
      // The image is the medium, and it is there from the moment the bay is
      // loaded: there are no tray or media-change events.
      CmdOk(ch);
      return;
    case kCdbRequestSense: {
      // 18-byte fixed-format sense data (§10.3.2). Bit 7 of byte 0 is the
      // "valid" flag; byte 7 is the additional sense length, so the ASC lands
      // in byte 12.
      int bytes = cdb[4] < 18 ? cdb[4] : 18;
      memset(ch->buf, 0, 18);
      ch->buf[0] = 0x70 | 0x80;
      ch->buf[2] = dr->sense_key;
      ch->buf[7] = 10;
      ch->buf[12] = dr->asc;
      StartDataIn(ch, 0, bytes, 0);
      return;
    }
    case kCdbInquiry: {
      int bytes = cdb[4] < kInquiryBytes ? cdb[4] : kInquiryBytes;
      BuildInquiry(ch->buf);
      StartDataIn(ch, 0, bytes, 0);
      return;
    }
    case kCdbStartStopUnit:
      // Load/eject are not modelled; the medium never leaves the bay.
      CmdOk(ch);
      return;
    case kCdbReadCapacity:
      // READ CAPACITY: the last addressable block and the block length, both
      // big-endian (§10.3.5).
      Put32Be(ch->buf, (uint32_t)(dr->blocks - 1));
      Put32Be(ch->buf + 4, kAtapiBlockSize);
      StartDataIn(ch, 0, 8, 0);
      return;
    case kCdbRead10:
    case kCdbRead12: {
      int64_t lba = ((int64_t)cdb[2] << 24) | ((int64_t)cdb[3] << 16) |
                    ((int64_t)cdb[4] << 8) | (int64_t)cdb[5];
      int64_t blocks = cdb[0] == kCdbRead10
                           ? ((int64_t)cdb[7] << 8) | (int64_t)cdb[8]
                           : ((int64_t)cdb[6] << 24) | ((int64_t)cdb[7] << 16) |
                                 ((int64_t)cdb[8] << 8) | (int64_t)cdb[9];
      if (blocks == 0) {  // "transfer nothing" is a successful no-op
        CmdOk(ch);
        return;
      }
      if (lba + blocks > dr->blocks) {
        AtapiError(ch, kSenseIllegalRequest, kAscLogicalBlockOutOfRange);
        return;
      }
      StartDataIn(ch, lba, (int)(blocks * kAtapiBlockSize), 1);
      return;
    }
    default:
      AtapiError(ch, kSenseIllegalRequest, kAscInvalidFieldInCdb);
      return;
  }
}

// A command register write starts a command on the selected drive, using the
// address registers the host wrote just before it. Commands complete inside the
// write — no busy window is modelled — which is invisible to the guests this
// board serves: they poll DRQ and the status register, and both already show
// the true state of the transfer.
static void ExecCommand(IdeChannel* ch, uint8_t cmd) {
  IdeDrive* dr = Selected(ch);
  int drive = ch->selected;
  ch->active = -1;
  ch->packet = 0;
  ch->left = 0;
  ch->round = 0;
  ch->used = 0;
  if (!dr->image) {
    // No drive in the bay: nothing answers. The command write leaves the status
    // register at zero — that is the one register an empty bay never changes,
    // and it is how both guests decide the bay is empty (seabios ata_detect
    // stops on a zero status; xv6 ide.c probes 0x1F7 for a nonzero reading
    // before it will touch disk 1).
    return;
  }
  if (dr->atapi) {
    switch (cmd) {
      case kCmdIdentifyPacket: {
        // The identity of a packet device travels as one 512-byte data-in block
        // (§7.10.2), which is what SeaBIOS's ata_detect reads before it will
        // call the bay a CD-ROM.
        uint16_t id[kIdentifyWords];
        BuildIdentifyPacket(id);
        memcpy(ch->buf, id, kAtaBlockSize);
        dr->error = 0;
        dr->status = kStatusReady;
        ch->active = drive;
        ch->is_write = 0;
        ch->refill = 0;
        ch->block_size = kAtaBlockSize;
        ch->limit = kAtaBlockSize;
        ch->lba = 0;
        ch->left = kAtaBlockSize;
        ch->count = kAtapiReasonIo;
        StartRound(ch);
        RaiseIrq(ch);
        return;
      }
      case kCmdPacket: {
        // PACKET (0xA0): the task file now carries the CDB. The byte count
        // limit in the cylinder registers is the most the host will take per
        // DRQ round (§9.6); a limit of 0xFFFF means "no limit" and is clamped
        // the way the reference implementation clamps it (QEMU hw/ide/core.c
        // ide_atapi_cmd_reply_end), and a limit the host never programmed does
        // not cap anything either.
        int limit = ch->lba_mid | (ch->lba_high << 8);
        if (limit == 0xffff) limit = 0xfffe;
        if (limit == 0) limit = kMaxBlockSize;
        ch->limit = limit;
        ch->packet = 1;
        ch->is_write = 1;
        ch->refill = 0;
        ch->block_size = kAtapiCdbSize;
        ch->lba = 0;
        ch->left = kAtapiCdbSize;
        ch->active = drive;
        dr->error = 0;
        dr->status = kStatusReady;
        ch->count = kAtapiReasonCd;  // the host is writing a command packet
        StartRound(ch);
        RaiseIrq(ch);
        return;
      }
      default:
        // The register command set belongs to a disk; a packet device answers
        // every one of its commands with ABRT (§9.5), which is exactly how
        // SeaBIOS's ata_detect learns to ask for IDENTIFY PACKET DEVICE instead.
        Abort(ch, dr);
        return;
    }
  }
  switch (cmd) {
    case kCmdIdentifyDevice: {
      uint16_t id[kIdentifyWords];
      BuildIdentify(ch, drive, id);
      memcpy(ch->buf, id, kAtaBlockSize);
      dr->error = 0;
      dr->status = kStatusReady;
      ch->active = drive;
      ch->is_write = 0;
      ch->refill = 0;
      ch->block_size = kAtaBlockSize;
      ch->limit = kAtaBlockSize;
      ch->lba = 0;
      ch->left = kAtaBlockSize;
      StartRound(ch);
      RaiseIrq(ch);
      return;
    }
    case kCmdReadSectors:
    case kCmdWriteSectors: {
      int ok = 0;
      int64_t lba = CommandLba(ch, dr, &ok);
      int count = ch->count ? ch->count : 256;  // 0 means 256 sectors (§7.10.4)
      if (!ok || lba < 0 || lba + count > dr->blocks) {
        Abort(ch, dr);
        return;
      }
      dr->error = 0;
      dr->status = kStatusReady;
      ch->active = drive;
      ch->is_write = cmd == kCmdWriteSectors;
      ch->refill = 1;
      ch->block_size = kAtaBlockSize;
      ch->limit = kAtaBlockSize;
      ch->lba = lba;
      ch->left = (int64_t)count * kAtaBlockSize;
      if (StartRound(ch) != 0) {
        MediumReadFailed(ch);
        return;
      }
      RaiseIrq(ch);
      return;
    }
    case kCmdFlushCache:
      // The medium is written through with every sector, so a flush is already
      // done: nothing to do but report success.
      dr->error = 0;
      dr->status = kStatusReady;
      RaiseIrq(ch);
      return;
    default:
      Abort(ch, dr);
      return;
  }
}

// Reading the status register is the drive's interrupt acknowledge: the request
// is retired and the line drops (§7.10.9; QEMU ide_status_read does the same,
// and it serves the alternate status port too).
static uint64_t ReadStatus(IdeChannel* ch) {
  ClearIrq(ch);
  return Selected(ch)->status;
}

static uint64_t CmdRead(void* dev, uint64_t addr, int size) {
  IdeChannel* ch = (IdeChannel*)dev;
  switch (addr - ch->cmd_base) {
    case kRegData:
      return ReadData(ch, size);
    case kRegFeatureError:
      return Selected(ch)->error;
    case kRegSectorCount:
      return ch->count;
    case kRegLbaLow:
      return ch->lba_low;
    case kRegLbaMid:
      return ch->lba_mid;
    case kRegLbaHigh:
      return ch->lba_high;
    case kRegDriveHead:
      return ch->head;
    default:
      return ReadStatus(ch);
  }
}

static void CmdWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  IdeChannel* ch = (IdeChannel*)dev;
  switch (addr - ch->cmd_base) {
    case kRegData:
      WriteData(ch, size, val);
      return;
    case kRegFeatureError:
      // The feature register is write-only, and the commands that consume it
      // (SET FEATURES, the LBA-48 extended commands) are not implemented, so
      // there is nothing to keep.
      return;
    case kRegSectorCount:
      ch->count = (uint8_t)val;
      return;
    case kRegLbaLow:
      ch->lba_low = (uint8_t)val;
      return;
    case kRegLbaMid:
      ch->lba_mid = (uint8_t)val;
      return;
    case kRegLbaHigh:
      ch->lba_high = (uint8_t)val;
      return;
    case kRegDriveHead:
      // The register holds the addressing bits and picks the bay the next
      // command executes on (bit 4). Firmware validates the controller by
      // writing 0xA0/0xB0 and reading the register back before it trusts the
      // status register (seabios ata_detect).
      ch->head = (uint8_t)val;
      ch->selected = (int)((val >> 4) & 1);
      PresentSignature(ch);
      return;
    default:
      ExecCommand(ch, (uint8_t)val);
      return;
  }
}

static uint64_t CtrlRead(void* dev, uint64_t addr, int size) {
  (void)addr;
  (void)size;  // the window is the alternate status register, one byte
  return ReadStatus((IdeChannel*)dev);
}

static void CtrlWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  IdeChannel* ch = (IdeChannel*)dev;
  (void)addr;
  (void)size;
  uint8_t old = ch->devctrl;
  ch->devctrl = (uint8_t)val;
  if (!(old & kDevCtrlSrst) && (ch->devctrl & kDevCtrlSrst)) {
    ResetChannel(ch);
    return;
  }
  // Clearing nIEN lets a request that arrived while interrupts were disabled
  // out (§7.10.10; QEMU re-evaluates the line after the same write).
  UpdateIrq(ch);
}

static const DeviceOps kIdeCmdOps = {"ide-taskfile", CmdRead, CmdWrite};
static const DeviceOps kIdeCtrlOps = {"ide-control", CtrlRead, CtrlWrite};

// BAR size decoding: the address bits below a BAR's window size are read-only
// zero, so firmware's size probe (write all ones, read back) sees the window
// size and alignment (PCI spec §6.2.5.1). Without this every BAR would answer
// "4GiB" and the resource allocator would try to place the windows itself.
static void MaskBar(uint8_t* cfg, uint32_t size) {
  uint32_t v = (uint32_t)cfg[0] | ((uint32_t)cfg[1] << 8) | ((uint32_t)cfg[2] << 16) |
               ((uint32_t)cfg[3] << 24);
  v = (v & ~(size - 1)) | 1;  // the I/O space indicator is read-only set
  cfg[0] = (uint8_t)v;
  cfg[1] = (uint8_t)(v >> 8);
  cfg[2] = (uint8_t)(v >> 16);
  cfg[3] = (uint8_t)(v >> 24);
}

static void ConfigWritten(void* ctx, uint8_t off, uint8_t val) {
  IdeDevice* d = (IdeDevice*)ctx;
  (void)val;
  switch (off & (uint8_t)~3u) {
    case kCfgBar0:
      MaskBar(&d->pci.config[kCfgBar0], kBarPrimaryCmdSize);
      break;
    case kCfgBar1:
      MaskBar(&d->pci.config[kCfgBar1], kBarPrimaryCtrlSize);
      break;
    case kCfgBar2:
      MaskBar(&d->pci.config[kCfgBar2], kBarSecondaryCmdSize);
      break;
    case kCfgBar3:
      MaskBar(&d->pci.config[kCfgBar3], kBarSecondaryCtrlSize);
      break;
    case kCfgBar4:
      // The bus master function is not implemented, so the BAR reads back as an
      // absent one (PCI spec §6.2.5.1: an unimplemented BAR returns zero).
      Put32(&d->pci.config[kCfgBar4], 0);
      break;
    default:
      break;
  }
}

void IdeInit(IdeDevice* d, uint8_t bus, uint8_t dev) {
  memset(d, 0, sizeof(*d));
  for (int i = 0; i < 2; i++) {
    IdeChannel* ch = &d->channels[i];
    ch->dev = d;
    ch->cmd_base = i == 0 ? kPrimaryCmdBase : kSecondaryCmdBase;
    ch->ctrl_base = i == 0 ? kPrimaryCtrlBase : kSecondaryCtrlBase;
    ch->irq_line = i == 0 ? kPrimaryIrq : kSecondaryIrq;
    ch->selected = 0;
    ch->active = -1;
    ch->block_size = kAtaBlockSize;
    ch->limit = kAtaBlockSize;
  }
  // The IDE function of the PCI-to-ISA bridge: function 1 of the bridge device
  // (QEMU hw/ide/piix.c puts it at 00:01.1 the same way).
  d->pci.bus = bus;
  d->pci.dev = dev;
  d->pci.func = 1;
  Put16(&d->pci.config[kCfgVendor], kVendorIntel);
  Put16(&d->pci.config[kCfgDevice], kDevicePiix3Ide);
  d->pci.config[kCfgRevision] = kRevision;
  d->pci.config[kCfgProgIf] = kProgIf;
  d->pci.config[kCfgSubClass] = kSubClassIde;
  d->pci.config[kCfgClass] = kClassStorage;
  d->pci.config[kCfgHeaderType] = 0;
  Put32(&d->pci.config[kCfgBar0], kBarPrimaryCmd);
  Put32(&d->pci.config[kCfgBar1], kBarPrimaryCtrl);
  Put32(&d->pci.config[kCfgBar2], kBarSecondaryCmd);
  Put32(&d->pci.config[kCfgBar3], kBarSecondaryCtrl);
  Put32(&d->pci.config[kCfgBar4], 0);
  // Compatibility mode: the channel interrupts are the fixed ISA IRQ 14/15, so
  // the function has no PCI interrupt pin (the PIIX3 datasheet says the same
  // for the IDE function while both channels stay compatible).
  d->pci.config[kCfgIntLine] = kIntLine;
  d->pci.config[kCfgIntPin] = 0;
  d->pci.written = ConfigWritten;
  d->pci.ctx = d;
}

int IdeAttach(IdeDevice* d, int channel, int drive, const char* path, int media) {
  if (channel < 0 || channel > 1 || drive < 0 || drive >= kIdeDrivesPerChannel) return -1;
  if (media != kIdeMediaDisk && media != kIdeMediaCd) return -1;
  IdeDrive* dr = &d->channels[channel].drives[drive];
  int block_size = media == kIdeMediaCd ? kAtapiBlockSize : kAtaBlockSize;
  HostFile* image = HostFileOpenReadWrite(path);
  if (!image) return -1;
  int64_t size = HostFileSize(image);
  if (size <= 0 || size % block_size != 0) {
    HostFileClose(image);
    return -1;
  }
  dr->image = image;
  dr->atapi = media == kIdeMediaCd;
  dr->blocks = size / block_size;
  // The LBA-assist geometry: 16 heads, 63 sectors per track, as many cylinders
  // as the medium needs (QEMU's hd_geometry_guess picks the same translation
  // for an image without a real drive geometry). Only a disk reports it: a CD
  // has no CHS geometry and is addressed by block number.
  dr->heads = 16;
  dr->sectors_per_track = 63;
  int64_t per_cylinder = (int64_t)dr->heads * dr->sectors_per_track;
  int64_t cylinders = (dr->blocks + per_cylinder - 1) / per_cylinder;
  dr->cylinders = cylinders > 0xffff ? 0xffff : (uint16_t)cylinders;
  dr->status = kStatusReady;
  PresentSignature(&d->channels[channel]);
  return 0;
}

void IdeSetIrqSink(IdeDevice* d, void (*set_irq)(void* ctx, int line, int level), void* ctx) {
  d->set_irq = set_irq;
  d->irq_ctx = ctx;
}

void IdeRegister(Bus* io, IdeDevice* d) {
  for (int i = 0; i < 2; i++) {
    IdeChannel* ch = &d->channels[i];
    BusAddRegion(io, ch->cmd_base, kCmdBlockSize, &kIdeCmdOps, ch);
    BusAddRegion(io, ch->ctrl_base, kCtrlBlockSize, &kIdeCtrlOps, ch);
  }
}

void IdeDestroy(IdeDevice* d) {
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j < kIdeDrivesPerChannel; j++) {
      HostFileClose(d->channels[i].drives[j].image);
      d->channels[i].drives[j].image = NULL;
    }
  }
}
