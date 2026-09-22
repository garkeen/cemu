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

// The ATA command set a disk implements (ATA-4). The extended (LBA-48) and
// queued commands are not implemented and abort, which is how a drive answers a
// command it does not support (§6.3 command protocol).
enum {
  kCmdReadSectors = 0x20,
  kCmdWriteSectors = 0x30,
  kCmdReadVerify = 0x40,
  kCmdInitializeParams = 0x91,
  kCmdReadDma = 0xc8,
  kCmdWriteDma = 0xca,
  kCmdStandbyImmediate = 0xe0,
  kCmdIdleImmediate = 0xe1,
  kCmdStandby = 0xe2,
  kCmdIdle = 0xe3,
  kCmdCheckPowerMode = 0xe5,
  kCmdSleep = 0xe6,
  kCmdFlushCache = 0xe7,
  kCmdIdentifyDevice = 0xec,
  kCmdSetFeatures = 0xef,
};

// Power management (§8.9, §8.12): what CHECK POWER MODE reports, and the state
// a sleeping drive is in until a reset wakes it.
enum {
  kPowerActive = 0xff,   // also "idle": the head is ready
  kPowerStandby = 0x00,
};
// The SET FEATURES subcommands this drive honours (§8.6). The transfer mode is
// whatever IDENTIFY advertised, the write cache writes through to the medium
// and read look-ahead never reorders anything — so these four ask for the state
// the drive is already in, and any other subcommand aborts.
enum {
  kFeatureEnableWriteCache = 0x02,
  kFeatureSetTransferMode = 0x03,
  kFeatureDisableLookAhead = 0x55,
  kFeatureEnableLookAhead = 0xaa,
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
// and reads (MMC), the mode pages and the disc structure a driver probes a
// CD-ROM with (SPC MODE SENSE; MMC-3 GET CONFIGURATION / READ TOC / READ CD),
// and the two commands that move the medium (MMC-3 START STOP UNIT,
// PREVENT/ALLOW MEDIUM REMOVAL).
enum {
  kCdbTestUnitReady = 0x00,
  kCdbRequestSense = 0x03,
  kCdbInquiry = 0x12,
  kCdbModeSense6 = 0x1a,
  kCdbStartStopUnit = 0x1b,
  kCdbPreventAllow = 0x1e,
  kCdbReadCapacity = 0x25,
  kCdbRead10 = 0x28,
  kCdbSeek = 0x2b,
  kCdbReadToc = 0x43,
  kCdbGetConfiguration = 0x46,
  kCdbModeSense10 = 0x5a,
  kCdbRead12 = 0xa8,
  kCdbReadCd = 0xbe,
};
// Sense keys and additional sense codes (§10.3.2, SPC sense data; the codes are
// the ones the reference implementation answers with, tiny386 ide.c /
// QEMU hw/ide/core.c).
enum {
  kSenseNoSense = 0x0,
  kSenseNotReady = 0x2,
  kSenseIllegalRequest = 0x5,
  // UNIT ATTENTION: raised when the medium may have changed, and collected by
  // REQUEST SENSE — while it is pending the device refuses everything else.
  kSenseUnitAttention = 0x6,
  kAscLogicalBlockOutOfRange = 0x21,
  kAscInvalidFieldInCdb = 0x24,
  kAscLunNotSupported = 0x25,
  kAscMediumMayHaveChanged = 0x28,
  kAscSavingParametersNotSupported = 0x39,
  kAscMediumNotPresent = 0x3a,
  kAscMediumRemovalPrevented = 0x53,
};
// MODE SENSE page codes: the read error recovery page and the CD capabilities /
// mechanical status page, the two a CD-ROM driver reads (SPC MODE SENSE; the
// same two the reference implementation answers).
enum {
  kModePageErrorRecovery = 0x01,
  kModePageCdCapabilities = 0x2a,
};
// MMC profile codes (MMC-3 GET CONFIGURATION): a data CD. There is no DVD
// profile — the medium is an ISO with 2048-byte blocks.
enum { kProfileCdRom = 0x0008 };
// One CD frame is 1/75 s and a track's first address is 00:02:00, i.e. frame
// 150 (MMC-3 "MSF addresses"; the reference implementation's lba_to_msf adds
// the same offset).
enum { kMsfFramesPerSecond = 75, kMsfFramesPerMinute = 75 * 60, kMsfPregap = 150 };
// The media-change bit of the error register, which a CHECK CONDITION that
// reports a unit attention carries alongside the sense key (§9.6 error
// register; QEMU ide_atapi_cmd_check_status).
enum { kErrorMc = 0x20 };
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

// The bus-master IDE window at BAR4 (PIIX3 "Bus Master IDE"; QEMU hw/ide/piix.c
// puts BMIBA at 0xc000, and libata's ata_piix programs the same offsets). One
// 8-byte register block per channel: the command and status registers are bytes
// at +0 and +2, and the descriptor table pointer is a dword at +4.
enum {
  kBmdmaBase = 0xc000,
  kBmdmaChannelStride = 8,
  kBarBusMaster = kBmdmaBase | 1,  // 16 ports, I/O space indicator set
  kBarBusMasterSize = 16,
  kBmdmaCmd = 0,
  kBmdmaStatus = 2,
  kBmdmaPrdTable = 4,
  // Command register: bit 0 starts and stops the engine; bit 3 is the transfer
  // direction — set means the engine writes to system memory, i.e. the drive is
  // being read (libata programs ATA_DMA_WR the same way for a device read).
  kBmdmaStart = 1 << 0,
  kBmdmaDirWriteMem = 1 << 3,
  // Status register: the engine's active bit, plus the error and interrupt bits
  // the host clears by writing ones back. Bits 7:6 are read-only capability
  // flags, one per bay.
  kBmdmaActive = 1 << 0,
  kBmdmaError = 1 << 1,
  kBmdmaIntr = 1 << 2,
  kBmdmaDrive0Dma = 1 << 7,
  kBmdmaDrive1Dma = 1 << 6,
  // A descriptor's second dword: bits 15:0 are the byte count (zero means a
  // full 64KiB region) and bit 31 ends the table (PIIX3 "Bus Master IDE
  // Descriptor Table").
  kBmdmaPrdFull = 0x10000,
  kBmdmaPrdEot = 1u << 31,
  kBmdmaPrdBytes = 8,
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
static void Put16Be(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

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
// software reset protocol). The tray is hardware, not a register: it keeps its
// position and its lock across a reset.
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
    // A reset spins a sleeping drive back up (§8.12).
    dr->power = kPowerActive;
    dr->sleeping = 0;
    // An empty bay keeps answering zero: that is how firmware and xv6 tell a
    // bay with no drive from one with a drive that has not been selected yet.
    dr->status = dr->image ? kStatusReady : 0;
  }
  ch->selected = 0;
  ch->active = -1;
  ch->is_write = 0;
  ch->packet = 0;
  ch->dma = 0;
  ch->left = 0;
  ch->round = 0;
  ch->used = 0;
  ch->refill = 0;
  ch->block_size = kAtaBlockSize;
  ch->limit = kAtaBlockSize;
  // A reset stops the bus-master engine and clears its interrupt (PIIX3 status
  // register; the descriptor pointer is the host's and survives).
  ch->bmdma_cmd = 0;
  ch->bmdma_status = 0;
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
// answers "not supported" to: 47/59 (R/W multiple), 88 (ultra DMA), 83 and
// 100..103 (LBA-48 capacity), 93 bits other than the ones below.
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
  // Word 49: capabilities — bit 9 = LBA supported, bit 8 = DMA supported (the
  // bus-master engine at BAR4 is what answers that).
  id[49] = 0x0300;
  // Word 53: bit 0 = words 54..58 carry data, bit 1 = words 64..70 do.
  id[53] = 0x0003;
  id[54] = dr->cylinders;
  id[55] = dr->heads;
  id[56] = dr->sectors_per_track;
  uint32_t chs_sectors = (uint32_t)dr->cylinders * dr->heads * dr->sectors_per_track;
  id[57] = (uint16_t)(chs_sectors & 0xffff);  // current capacity, words 57..58
  id[58] = (uint16_t)(chs_sectors >> 16);
  id[60] = (uint16_t)(dr->blocks & 0xffff);  // LBA-28 capacity, words 60..61
  id[61] = (uint16_t)((dr->blocks >> 16) & 0xffff);
  // Words 63..68: the transfer modes on offer and their timings. Word 63 lists
  // the multiword DMA modes (bit i = mode i) and word 64 the advanced PIO
  // modes; 65/66 are the minimum and recommended multiword DMA cycle times and
  // 67/68 the PIO cycle times without and with flow control. The engine moves
  // the data as fast as the host asks, so the numbers are the ones the modes
  // are defined with (mode 2 = 120 ns per word, a 240 ns PIO cycle).
  id[63] = 0x0007;  // multiword DMA modes 0, 1 and 2
  id[64] = 0x0007;  // advanced PIO modes 0, 1 and 2
  id[65] = 0x0078;
  id[66] = 0x0078;
  id[67] = 0x00f0;
  id[68] = 0x00f0;
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
  // Word 49: capabilities — bit 9 = LBA supported, bit 8 = DMA supported (the
  // bus-master engine at BAR4 is what answers that).
  id[49] = 0x0300;
  // Word 53: bit 0 = words 54..58 carry data, bit 1 = words 64..70 carry data.
  id[53] = 0x0003;
  // Word 63 lists the multiword DMA modes (bit i = mode i) and word 64 the
  // advanced PIO modes; words 65/66 give the multiword DMA cycle times and
  // 67/68 the PIO cycle times without and with flow control. The engine moves
  // the data as fast as the host asks, so the numbers are the ones the modes
  // are defined with.
  id[63] = 0x0007;  // multiword DMA modes 0, 1 and 2
  id[64] = 0x0007;  // advanced PIO modes 0, 1 and 2
  id[65] = 0x0078;
  id[66] = 0x0078;
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
  // A bus-master transfer does not hand the data over through DRQ: the engine
  // moves it, and the status register shows no request while it does.
  if (!ch->dma) ch->drives[ch->active].status |= kStatusDrq;
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
  if (ch->dma) return 0;  // the engine is moving this transfer, not the port
  if (ch->active < 0 || ch->is_write || ch->round < size) return 0;
  uint64_t v = 0;
  for (int i = 0; i < size; i++) v |= (uint64_t)ch->buf[ch->used + i] << (8 * i);
  MoveBytes(ch, size);
  return v;
}

static void WriteData(IdeChannel* ch, int size, uint64_t val) {
  // A bus-master transfer does not go through the data port: the engine moves
  // it, so a port access while the engine runs is a host error and moves
  // nothing.
  if (ch->dma) return;
  if (ch->active < 0 || !ch->is_write || ch->round < size) return;
  for (int i = 0; i < size; i++) ch->buf[ch->used + i] = (uint8_t)(val >> (8 * i));
  MoveBytes(ch, size);
}

// ---- bus-master DMA ---------------------------------------------------------

// The engine's view of system memory: the descriptor table and the host's data
// buffers are both guest RAM, which is why the controller needs the bus at all.
static uint32_t MemRead32(const IdeChannel* ch, uint32_t addr) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++)
    v |= (uint32_t)BusRead(ch->dev->mem, addr + (uint32_t)i, 1) << (8 * i);
  return v;
}

static void MemMove(IdeChannel* ch, uint32_t addr, uint8_t* buf, int n, int to_mem) {
  for (int i = 0; i < n; i++) {
    if (to_mem)
      BusWrite(ch->dev->mem, addr + (uint32_t)i, 1, buf[i]);
    else
      buf[i] = (uint8_t)BusRead(ch->dev->mem, addr + (uint32_t)i, 1);
  }
}

// Whether the engine can run at all: it moves data through system memory, so a
// board that never handed the controller an address space has no DMA, and says
// so in the status register's capability bits rather than accepting a start it
// could not honour.
static int BmdmaUsable(const IdeChannel* ch) { return ch->dev->mem != NULL; }

// The end of a bus-master transfer: the engine stops (its start bit clears and
// its active bit drops), the device reports completion, and the interrupt bit
// records that the drive raised INTR (PIIX3 status register).
static void BmdmaFinish(IdeChannel* ch) {
  ch->dma = 0;
  ch->bmdma_cmd &= (uint8_t)~kBmdmaStart;
  ch->bmdma_status = (uint8_t)((ch->bmdma_status & ~kBmdmaActive) | kBmdmaIntr);
  EndCommand(ch);
}

// One bus-master transfer. The descriptor table names the system-memory regions
// the data moves through and the command register's direction bit says which
// way; the descriptor marked EOT ends the table. The whole transfer runs inside
// the start write — the same "no busy window" model the task file uses — and
// the medium is refilled or written block by block exactly as the PIO path does
// it, so both protocols move identical bytes.
static void BmdmaRun(IdeChannel* ch) {
  IdeDrive* dr = &ch->drives[ch->active];
  int to_mem = (ch->bmdma_cmd & kBmdmaDirWriteMem) != 0;
  uint32_t prd = ch->bmdma_prd;
  int failed = 0;
  ch->dma = 1;
  while (ch->left > 0 && !failed) {
    uint32_t addr = MemRead32(ch, prd) & ~1u;
    uint32_t second = MemRead32(ch, prd + 4);
    uint32_t count = second & 0xffff;
    if (count == 0) count = kBmdmaPrdFull;
    for (uint32_t off = 0; off < count && ch->left > 0;) {
      int64_t n = (int64_t)(count - off);
      int64_t block_left = ch->block_size - ch->used;
      if (n > block_left) n = block_left;
      if (n > ch->left) n = ch->left;
      if (to_mem && ch->used == 0 && ch->refill &&
          HostFileReadAt(dr->image, ch->lba * ch->block_size, ch->buf,
                         (size_t)ch->block_size) != (size_t)ch->block_size) {
        failed = 1;
        break;
      }
      MemMove(ch, addr + off, ch->buf + ch->used, (int)n, to_mem);
      ch->used += (int)n;
      ch->left -= n;
      off += (uint32_t)n;
      if (ch->used == ch->block_size) {
        if (!to_mem &&
            HostFileWriteAt(dr->image, ch->lba * ch->block_size, ch->buf,
                            (size_t)ch->block_size) != (size_t)ch->block_size) {
          failed = 1;
          break;
        }
        ch->used = 0;
        ch->lba++;
      }
    }
    if (failed || (second & kBmdmaPrdEot)) break;
    prd += kBmdmaPrdBytes;
  }
  ch->bmdma_cmd &= (uint8_t)~kBmdmaStart;
  if (failed) {
    // A medium that cannot be read or written where the command asked: the
    // engine reports the error and the device the sense, exactly as the PIO
    // path reports them.
    ch->bmdma_status = (uint8_t)((ch->bmdma_status & ~kBmdmaActive) | kBmdmaError);
    ch->dma = 0;
    MediumReadFailed(ch);
    return;
  }
  if (ch->left > 0) {
    // The table ran out before the transfer did: the host under-programmed it,
    // which the engine reports as an error rather than as a short transfer.
    ch->bmdma_status = (uint8_t)((ch->bmdma_status & ~kBmdmaActive) | kBmdmaError);
    ch->dma = 0;
    Abort(ch, dr);
    return;
  }
  BmdmaFinish(ch);
}

// The bus-master window: one 8-byte block per channel.
static uint64_t BmdmaRead(void* dev, uint64_t addr, int size) {
  IdeChannel* ch = (IdeChannel*)dev;
  (void)size;
  switch (addr & 7) {
    case kBmdmaCmd:
      return ch->bmdma_cmd;
    case kBmdmaStatus: {
      uint8_t caps = 0;
      if (BmdmaUsable(ch)) {
        if (ch->drives[0].image) caps |= kBmdmaDrive0Dma;
        if (ch->drives[1].image) caps |= kBmdmaDrive1Dma;
      }
      return (uint8_t)(ch->bmdma_status | caps);
    }
    case kBmdmaPrdTable:
    case kBmdmaPrdTable + 1:
    case kBmdmaPrdTable + 2:
    case kBmdmaPrdTable + 3:
      return (ch->bmdma_prd >> ((addr & 7) - kBmdmaPrdTable) * 8) & 0xff;
    default:
      return 0;  // the reserved bytes read zero
  }
}

static void BmdmaWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  IdeChannel* ch = (IdeChannel*)dev;
  (void)size;
  switch (addr & 7) {
    case kBmdmaCmd: {
      int was = (ch->bmdma_cmd & kBmdmaStart) != 0;
      ch->bmdma_cmd = (uint8_t)(val & (kBmdmaStart | kBmdmaDirWriteMem));
      if (!was && (ch->bmdma_cmd & kBmdmaStart)) {
        // Starting the engine moves whatever the active command still has to
        // move. It may be started before the command runs (a driver that sets
        // the engine up first) or while its data phase waits on DRQ; both are
        // the same hardware event, and with nothing active there is nothing to
        // move.
        if (ch->active >= 0 && ch->left > 0 && ch->dev->mem) {
          ch->bmdma_status |= kBmdmaActive;
          BmdmaRun(ch);
        }
      } else if (was && !(ch->bmdma_cmd & kBmdmaStart)) {
        ch->bmdma_status &= (uint8_t)~kBmdmaActive;  // the host stopped it
      }
      return;
    }
    case kBmdmaStatus:
      // Error and interrupt are write-one-to-clear; the active bit is the
      // engine's own state (PIIX3 status register).
      ch->bmdma_status = (uint8_t)(ch->bmdma_status & ~((uint8_t)val & (kBmdmaError | kBmdmaIntr)));
      return;
    case kBmdmaPrdTable:
    case kBmdmaPrdTable + 1:
    case kBmdmaPrdTable + 2:
    case kBmdmaPrdTable + 3: {
      int shift = ((int)(addr & 7) - kBmdmaPrdTable) * 8;
      ch->bmdma_prd = (ch->bmdma_prd & ~(0xffu << shift)) | ((uint32_t)(val & 0xff) << shift);
      return;
    }
    default:
      return;  // the reserved bytes are dropped
  }
}

static const DeviceOps kBmdmaOps = {"ide-bmdma", BmdmaRead, BmdmaWrite};

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
  // Which protocol carries the data is the host's choice, not the CDB's: if it
  // started the bus-master engine before the command ran, the transfer moves
  // through the engine, whose own completion interrupt ends the command.
  ch->dma = (ch->bmdma_cmd & kBmdmaStart) != 0 && BmdmaUsable(ch);
  if (ch->dma) {
    ch->count = kAtapiReasonIo;
    BmdmaRun(ch);
    return;
  }
  if (StartRound(ch) != 0) {
    MediumReadFailed(ch);
    return;
  }
  ch->count = kAtapiReasonIo;
  RaiseIrq(ch);
}

// A packet reply carries at most the allocation length the host asked for
// (QEMU hw/ide/core.c ide_atapi_cmd_reply clamps the same way).
static int ClampReply(int bytes, int max_len) { return bytes < max_len ? bytes : max_len; }

// The medium a packet command needs. The attached image is the medium, and it
// is absent while the tray is open (MMC-3 START STOP UNIT with LoEj and no
// load) — a command that needs the medium then answers NOT READY / MEDIUM NOT
// PRESENT, which is how a driver learns the bay is empty.
static int MediumPresent(const IdeDrive* dr) { return dr->image && !dr->tray_open; }

// CHECK CONDITION with a unit attention still pending: the command is refused
// without being executed, and only REQUEST SENSE (which collects it) or INQUIRY
// gets through (QEMU hw/ide/core.c ide_atapi_cmd_check_status). The error
// register carries the media-change bit next to the sense key.
static void CheckCondition(IdeChannel* ch) {
  IdeDrive* dr = &ch->drives[ch->active];
  dr->error = (uint8_t)(kErrorMc | (kSenseUnitAttention << 4));
  dr->status = kStatusErr;
  ch->count = 0;
  ch->active = -1;
  ch->left = 0;
  ch->round = 0;
  ch->packet = 0;
  RaiseIrq(ch);
}

// A minute:second:frame address, which is what a TOC descriptor carries instead
// of a block number when the host asks for MSF (MMC-3).
static void PutMsf(uint8_t* p, uint32_t block) {
  uint32_t frames = block + kMsfPregap;
  p[0] = (uint8_t)(frames / kMsfFramesPerMinute);
  p[1] = (uint8_t)((frames / kMsfFramesPerSecond) % 60);
  p[2] = (uint8_t)(frames % kMsfFramesPerSecond);
}

// MODE SENSE: a mode parameter header followed by one mode page (SPC MODE
// SENSE). Two pages are offered, the pair a CD-ROM driver reads: the read error
// recovery page and the CD capabilities / mechanical status page. The header is
// the 8-byte form for both the 6- and the 10-byte CDB, which is what the
// reference implementation answers and what drivers written against it expect.
static void ModeSense(IdeChannel* ch) {
  const uint8_t* cdb = ch->buf;
  int ten = cdb[0] == kCdbModeSense10;
  int max_len = ten ? (((int)cdb[7] << 8) | cdb[8]) : cdb[4];
  int action = cdb[2] >> 6;
  int code = cdb[2] & 0x3f;
  if (action == 3) {  // saved values: this device never saves any
    AtapiError(ch, kSenseIllegalRequest, kAscSavingParametersNotSupported);
    return;
  }
  if (action != 0) {  // changeable and default values are not offered
    AtapiError(ch, kSenseIllegalRequest, kAscInvalidFieldInCdb);
    return;
  }
  memset(ch->buf, 0, 32);
  switch (code) {
    case kModePageErrorRecovery:
      Put16Be(ch->buf, 22);   // mode data length: 6 header + 16 page
      ch->buf[2] = 0x70;      // medium type: CD-ROM, not write protected
      ch->buf[8] = kModePageErrorRecovery;
      ch->buf[9] = 6;         // page length
      ch->buf[11] = 0x05;     // read retry count
      StartDataIn(ch, 0, ClampReply(16, max_len), 0);
      return;
    case kModePageCdCapabilities:
      Put16Be(ch->buf, 34);   // mode data length: 6 header + 28 page
      ch->buf[2] = 0x70;
      ch->buf[8] = kModePageCdCapabilities;
      ch->buf[9] = 18;        // page length
      // Capabilities: the flags a driver checks before it will mount the medium
      // (MMC-3 "CD capabilities and mechanical status page"; the bits are the
      // reference implementation's, including the audio-play flag some Linux
      // code checks before it automounts).
      ch->buf[12] = 0x71;
      ch->buf[13] = 3 << 5;
      ch->buf[14] = (1 << 0) | (1 << 3) | (1 << 5);
      Put16Be(ch->buf + 16, 706);  // maximum read speed, kB/s (1x = 176)
      ch->buf[19] = 2;             // number of volume levels
      Put16Be(ch->buf + 20, 512);  // buffer size, kB
      Put16Be(ch->buf + 22, 706);  // current read speed
      StartDataIn(ch, 0, ClampReply(28, max_len), 0);
      return;
    default:
      AtapiError(ch, kSenseIllegalRequest, kAscInvalidFieldInCdb);
      return;
  }
}

// GET CONFIGURATION: the feature list of a data CD (MMC-3). Only feature 0 —
// the profile list — is offered, so a request for any other feature is refused
// the way the reference implementation refuses it.
static void GetConfiguration(IdeChannel* ch) {
  const uint8_t* cdb = ch->buf;
  if (cdb[2] != 0 || cdb[3] != 0) {
    AtapiError(ch, kSenseIllegalRequest, kAscInvalidFieldInCdb);
    return;
  }
  int max_len = ((int)cdb[7] << 8) | cdb[8];
  if (max_len > kAtaBlockSize) max_len = kAtaBlockSize;
  memset(ch->buf, 0, 16);
  // The current profile is a data CD; the same profile is the single entry of
  // the list, flagged as current.
  Put16Be(ch->buf + 6, kProfileCdRom);
  ch->buf[10] = 0x02 | 0x01;  // the feature is persistent and current
  ch->buf[11] = 4;            // additional length: one 4-byte profile entry
  Put16Be(ch->buf + 12, kProfileCdRom);
  ch->buf[14] = 0x01;         // this entry is the current profile
  Put32Be(ch->buf, 12);       // data length: the reply minus this field
  StartDataIn(ch, 0, ClampReply(16, max_len), 0);
}

// The track descriptors of a data CD: one data track at block 0 and the
// lead-out at the end of the medium (MMC-3 READ TOC format 0). Returns the
// reply length, or -1 for a starting track the disc does not have.
static int BuildToc(const IdeDrive* dr, uint8_t* buf, int msf, int start_track) {
  if (start_track > 1 && start_track != 0xaa) return -1;
  uint8_t* q = buf + 2;
  *q++ = 1;  // first session
  *q++ = 1;  // last session
  if (start_track <= 1) {
    *q++ = 0;     // reserved
    *q++ = 0x14;  // ADR = 1 (position data), control = data track
    *q++ = 1;     // track number
    *q++ = 0;     // reserved
    if (msf) {
      *q++ = 0;
      PutMsf(q, 0);
      q += 3;
    } else {
      Put32Be(q, 0);
      q += 4;
    }
  }
  *q++ = 0;     // lead-out
  *q++ = 0x16;
  *q++ = 0xaa;
  *q++ = 0;
  if (msf) {
    *q++ = 0;
    PutMsf(q, (uint32_t)dr->blocks);
    q += 3;
  } else {
    Put32Be(q, (uint32_t)dr->blocks);
    q += 4;
  }
  int len = (int)(q - buf);
  Put16Be(buf, (uint16_t)(len - 2));
  return len;
}

// The raw table of contents (MMC-3 READ TOC format 2): the four descriptors a
// driver walks to find the first and last track and the lead-in/lead-out
// (the reference implementation's cdrom_read_toc_raw).
static int BuildRawToc(const IdeDrive* dr, uint8_t* buf, int msf) {
  uint8_t* q = buf + 2;
  *q++ = 1;  // first session
  *q++ = 1;  // last session
  *q++ = 1;      // session number
  *q++ = 0x14;   // ADR, control
  *q++ = 0;      // track number
  *q++ = 0xa0;   // point: first track
  *q++ = 0;      // min
  *q++ = 0;      // sec
  *q++ = 0;      // frame
  *q++ = 0;
  *q++ = 1;      // first track number
  *q++ = 0;      // disc type
  *q++ = 0;

  *q++ = 1;
  *q++ = 0x14;
  *q++ = 0;
  *q++ = 0xa1;   // point: last track
  *q++ = 0;
  *q++ = 0;
  *q++ = 0;
  *q++ = 0;
  *q++ = 1;      // last track number
  *q++ = 0;
  *q++ = 0;

  *q++ = 1;
  *q++ = 0x14;
  *q++ = 0;
  *q++ = 0xa2;   // point: lead-out
  *q++ = 0;
  *q++ = 0;
  *q++ = 0;
  if (msf) {
    *q++ = 0;
    PutMsf(q, (uint32_t)dr->blocks);
    q += 3;
  } else {
    Put32Be(q, (uint32_t)dr->blocks);
  }

  *q++ = 1;
  *q++ = 0x14;
  *q++ = 0;
  *q++ = 1;      // point: track 1
  *q++ = 0;
  *q++ = 0;
  *q++ = 0;
  if (msf) {
    *q++ = 0;
    PutMsf(q, 0);
    q += 3;
  } else {
    Put32Be(q, 0);
  }
  int len = (int)(q - buf);
  Put16Be(buf, (uint16_t)(len - 2));
  return len;
}

static void ReadToc(IdeChannel* ch) {
  const uint8_t* cdb = ch->buf;
  IdeDrive* dr = &ch->drives[ch->active];
  if (!MediumPresent(dr)) {
    AtapiError(ch, kSenseNotReady, kAscMediumNotPresent);
    return;
  }
  int max_len = ((int)cdb[7] << 8) | cdb[8];
  int msf = (cdb[1] >> 1) & 1;
  switch (cdb[9] >> 6) {
    case 0: {
      int len = BuildToc(dr, ch->buf, msf, cdb[6]);
      if (len < 0) {
        AtapiError(ch, kSenseIllegalRequest, kAscInvalidFieldInCdb);
        return;
      }
      StartDataIn(ch, 0, ClampReply(len, max_len), 0);
      return;
    }
    case 1:
      // Multi session: the medium has a single session.
      memset(ch->buf, 0, 12);
      ch->buf[1] = 0x0a;
      ch->buf[2] = 0x01;
      ch->buf[3] = 0x01;
      StartDataIn(ch, 0, ClampReply(12, max_len), 0);
      return;
    case 2: {
      int len = BuildRawToc(dr, ch->buf, msf);
      StartDataIn(ch, 0, ClampReply(len, max_len), 0);
      return;
    }
    default:
      AtapiError(ch, kSenseIllegalRequest, kAscInvalidFieldInCdb);
      return;
  }
}

// READ CD (MMC-3): the same medium read as READ(10), but with the sector type
// selected by the CDB. A data CD carries 2048-byte user blocks, which is what
// "normal read" asks for; "read all data" asks for the 2352-byte raw sector
// including its sync, header and EDC, and an ISO image does not hold those
// bytes, so that request is refused rather than answered with data the medium
// never had.
static void ReadCd(IdeChannel* ch) {
  const uint8_t* cdb = ch->buf;
  IdeDrive* dr = &ch->drives[ch->active];
  if (!MediumPresent(dr)) {
    AtapiError(ch, kSenseNotReady, kAscMediumNotPresent);
    return;
  }
  int64_t blocks = ((int64_t)cdb[6] << 16) | ((int64_t)cdb[7] << 8) | cdb[8];
  int64_t lba = ((int64_t)cdb[2] << 24) | ((int64_t)cdb[3] << 16) | ((int64_t)cdb[4] << 8) | cdb[5];
  if (blocks == 0) {
    CmdOk(ch);
    return;
  }
  if (cdb[9] & 0xf8) {
    AtapiError(ch, kSenseIllegalRequest, kAscInvalidFieldInCdb);
    return;
  }
  if (cdb[9] == 0) {  // the host asked for no data at all
    CmdOk(ch);
    return;
  }
  if (lba + blocks > dr->blocks) {
    AtapiError(ch, kSenseIllegalRequest, kAscLogicalBlockOutOfRange);
    return;
  }
  StartDataIn(ch, lba, (int)(blocks * kAtapiBlockSize), 1);
}

// SEEK(10): position the head. There is no head to move, but the address is
// still validated, which is the whole point of the command for a driver
// checking whether a block is on the medium (MMC-3 SEEK).
static void Seek(IdeChannel* ch) {
  const uint8_t* cdb = ch->buf;
  IdeDrive* dr = &ch->drives[ch->active];
  if (!MediumPresent(dr)) {
    AtapiError(ch, kSenseNotReady, kAscMediumNotPresent);
    return;
  }
  int64_t lba = ((int64_t)cdb[2] << 24) | ((int64_t)cdb[3] << 16) | ((int64_t)cdb[4] << 8) | cdb[5];
  if (lba >= dr->blocks) {
    AtapiError(ch, kSenseIllegalRequest, kAscLogicalBlockOutOfRange);
    return;
  }
  CmdOk(ch);
}

// PREVENT/ALLOW MEDIUM REMOVAL (MMC-3): the lock the tray's eject path checks.
static void PreventAllow(IdeChannel* ch) {
  IdeDrive* dr = &ch->drives[ch->active];
  dr->tray_locked = ch->buf[4] & 1;
  CmdOk(ch);
}

// START STOP UNIT (MMC-3): the load/eject command. LoEj moves the tray, and the
// medium coming or going is the one event that raises a unit attention — the
// driver has to collect it with REQUEST SENSE before the device will do
// anything else.
static void StartStopUnit(IdeChannel* ch) {
  IdeDrive* dr = &ch->drives[ch->active];
  int start = ch->buf[4] & 1;
  int loej = (ch->buf[4] >> 1) & 1;
  if (loej) {
    if (!start && !dr->tray_open && dr->tray_locked) {
      AtapiError(ch, kSenseNotReady, kAscMediumRemovalPrevented);
      return;
    }
    if (start != !dr->tray_open) {
      dr->tray_open = !start;
      dr->sense_key = kSenseUnitAttention;
      dr->asc = kAscMediumMayHaveChanged;
    }
  }
  CmdOk(ch);
}

// The CDB a PACKET command just collected decides what the device does
// (ATA/ATAPI-7 §10 packet command set; SPC for INQUIRY and REQUEST SENSE, MMC
// for READ CAPACITY and the READ family).
static void ExecPacket(IdeChannel* ch) {
  IdeDrive* dr = &ch->drives[ch->active];
  const uint8_t* cdb = ch->buf;
  // LUN: the device answers for LUN 0 only, and a CDB naming another one is
  // refused before it is looked at (SPC-3: LOGICAL UNIT NOT SUPPORTED).
  if ((cdb[1] >> 5) != 0) {
    AtapiError(ch, kSenseIllegalRequest, kAscLunNotSupported);
    return;
  }
  // A unit attention outranks the command: until the guest collects it with
  // REQUEST SENSE, only REQUEST SENSE and INQUIRY complete (QEMU hw/ide/core.c
  // ide_atapi_cmd's gate).
  if (dr->sense_key == kSenseUnitAttention && cdb[0] != kCdbRequestSense &&
      cdb[0] != kCdbInquiry) {
    CheckCondition(ch);
    return;
  }
  switch (cdb[0]) {
    case kCdbTestUnitReady:
      // The image is the medium, and it is there from the moment the bay is
      // loaded — but a tray cycle is a change, and a device that has just seen
      // one reports the bay as not ready until the guest has asked what
      // happened (SPC TEST UNIT READY).
      if (!MediumPresent(dr)) {
        AtapiError(ch, kSenseNotReady, kAscMediumNotPresent);
        return;
      }
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
      StartStopUnit(ch);
      return;
    case kCdbPreventAllow:
      PreventAllow(ch);
      return;
    case kCdbModeSense6:
    case kCdbModeSense10:
      ModeSense(ch);
      return;
    case kCdbGetConfiguration:
      GetConfiguration(ch);
      return;
    case kCdbReadToc:
      ReadToc(ch);
      return;
    case kCdbSeek:
      Seek(ch);
      return;
    case kCdbReadCd:
      ReadCd(ch);
      return;
    case kCdbReadCapacity:
      if (!MediumPresent(dr)) {
        AtapiError(ch, kSenseNotReady, kAscMediumNotPresent);
        return;
      }
      // READ CAPACITY: the last addressable block and the block length, both
      // big-endian (§10.3.5).
      Put32Be(ch->buf, (uint32_t)(dr->blocks - 1));
      Put32Be(ch->buf + 4, kAtapiBlockSize);
      StartDataIn(ch, 0, 8, 0);
      return;
    case kCdbRead10:
    case kCdbRead12: {
      if (!MediumPresent(dr)) {
        AtapiError(ch, kSenseNotReady, kAscMediumNotPresent);
        return;
      }
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
  if (dr->sleeping) {
    // A sleeping drive answers nothing at all until a reset wakes it (§8.12):
    // the command is dropped and the status register keeps showing BSY.
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
    case kCmdWriteSectors:
    case kCmdReadDma:
    case kCmdWriteDma: {
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
      ch->is_write = cmd == kCmdWriteSectors || cmd == kCmdWriteDma;
      ch->refill = 1;
      ch->block_size = kAtaBlockSize;
      ch->limit = kAtaBlockSize;
      ch->lba = lba;
      ch->left = (int64_t)count * kAtaBlockSize;
      // The DMA commands move through the bus-master engine, which the host
      // starts after it writes the command: nothing is handed over by DRQ, and
      // the engine's own completion is what ends the command.
      ch->dma = cmd == kCmdReadDma || cmd == kCmdWriteDma;
      if (StartRound(ch) != 0) {
        MediumReadFailed(ch);
        return;
      }
      RaiseIrq(ch);
      return;
    }
    case kCmdReadVerify: {
      int ok = 0;
      int64_t lba = CommandLba(ch, dr, &ok);
      int count = ch->count ? ch->count : 256;
      if (!ok || lba < 0 || lba + count > dr->blocks) {
        Abort(ch, dr);
        return;
      }
      // READ VERIFY reads the medium and throws the data away: the transfer is
      // the same one a read makes, the destination is not (§8.4).
      for (int i = 0; i < count; i++) {
        if (HostFileReadAt(dr->image, (lba + (int64_t)i) * kAtaBlockSize, ch->buf,
                           kAtaBlockSize) != (size_t)kAtaBlockSize) {
          Abort(ch, dr);
          return;
        }
      }
      dr->error = 0;
      dr->status = kStatusReady;
      RaiseIrq(ch);
      return;
    }
    case kCmdInitializeParams: {
      // The geometry the task file names becomes the one IDENTIFY reports and
      // the one CHS commands translate through (§8.5).
      if (ch->count == 0 || ch->lba_low == 0) {
        Abort(ch, dr);
        return;
      }
      dr->sectors_per_track = ch->count;
      dr->heads = ch->lba_low;
      int64_t per_cylinder = (int64_t)dr->heads * dr->sectors_per_track;
      int64_t cylinders = (dr->blocks + per_cylinder - 1) / per_cylinder;
      dr->cylinders = cylinders > 0xffff ? 0xffff : (uint16_t)cylinders;
      dr->error = 0;
      dr->status = kStatusReady;
      RaiseIrq(ch);
      return;
    }
    case kCmdSetFeatures:
      // The four subcommands this drive honours ask for the state it is already
      // in — the transfer mode IDENTIFY advertised, a write cache that writes
      // through, and a read path that never reorders — so they succeed; any
      // other subcommand aborts, the way a drive answers one it does not
      // implement (§8.6).
      switch (ch->feature) {
        case kFeatureSetTransferMode:
        case kFeatureEnableWriteCache:
        case kFeatureEnableLookAhead:
        case kFeatureDisableLookAhead:
          dr->error = 0;
          dr->status = kStatusReady;
          RaiseIrq(ch);
          return;
        default:
          Abort(ch, dr);
          return;
      }
    case kCmdCheckPowerMode:
      // CHECK POWER MODE answers in the sector count register (§8.9).
      ch->count = dr->power;
      dr->error = 0;
      dr->status = kStatusReady;
      RaiseIrq(ch);
      return;
    case kCmdIdle:
    case kCmdIdleImmediate:
      dr->power = kPowerActive;
      dr->error = 0;
      dr->status = kStatusReady;
      RaiseIrq(ch);
      return;
    case kCmdStandby:
    case kCmdStandbyImmediate:
      dr->power = kPowerStandby;
      dr->error = 0;
      dr->status = kStatusReady;
      RaiseIrq(ch);
      return;
    case kCmdSleep:
      // A sleeping drive spins down and answers nothing but a reset (§8.12).
      dr->power = kPowerStandby;
      dr->sleeping = 1;
      dr->status = kStatusBsy;
      return;
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
      // The feature register is write-only; SET FEATURES is the command that
      // reads back what the host put there.
      ch->feature = (uint8_t)val;
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
      // The bus-master window is 16 ports, and the address bits below them are
      // read-only zero, so firmware's size probe sees the window (PCI spec
      // §6.2.5.1).
      MaskBar(&d->pci.config[kCfgBar4], kBarBusMasterSize);
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
  Put32(&d->pci.config[kCfgBar4], kBarBusMaster);
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
  dr->power = kPowerActive;
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
    // The bus-master window (BAR4): one 8-byte register block per channel.
    BusAddRegion(io, kBmdmaBase + (uint64_t)i * kBmdmaChannelStride, kBmdmaChannelStride,
                 &kBmdmaOps, ch);
  }
}

void IdeSetDmaBus(IdeDevice* d, Bus* mem) { d->mem = mem; }

void IdeDestroy(IdeDevice* d) {
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j < kIdeDrivesPerChannel; j++) {
      HostFileClose(d->channels[i].drives[j].image);
      d->channels[i].drives[j].image = NULL;
    }
  }
}
