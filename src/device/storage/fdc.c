#include "device/storage/fdc.h"

#include <string.h>

#include "debug/debug.h"
#include "util/log.h"

// Ports. 0x3F6 sits between the register file and the digital input register
// but belongs to the IDE controller's control block, so the window is split
// in two (PC/AT Technical Reference; QEMU hw/block/fdc.c maps the same set
// with 0x3F6 claimed by ide).
enum {
  kFdcBase = 0x3f0,
  kFdcRegCount = 6,  // 0x3F0-0x3F5
  kFdcDirPort = 0x3f7,
};
enum {
  kRegSra = 0,
  kRegSrb = 1,
  kRegDor = 2,
  kRegTdr = 3,
  kRegMsr = 4,   // read
  kRegFifo = 5,
  kRegDir = 7,  // read
};

// Status Register A (8272A datasheet).
enum { kSraIntPend = 0x80 };
// Status Register B: the reset value is the chip's signature after a reset.
enum {
  kSrbReset = 0xc0,
  kSrbMotor0 = 0x01,
  kSrbMotor1 = 0x02,
  kSrbDrive0 = 0x20,
};
// Digital Output Register: drive select, reset line, DMA/IRQ enable, motors.
enum {
  kDorSelect = 0x03,
  kDorNReset = 0x04,
  kDorDmaEn = 0x08,
  kDorMotor0 = 0x10,
  kDorMotor1 = 0x20,
};
// Main Status Register (8272A datasheet): RQM gates every access, DIO says
// which way the data register is facing, CMD BUSY covers the time between the
// last command byte and the last result byte.
enum {
  kMsrCmdBusy = 0x10,
  kMsrNonDma = 0x20,
  kMsrDio = 0x40,
  kMsrRqm = 0x80,
};
// Digital Input Register bit 7: no disk, or the disk was changed since the
// last step (SeaBIOS src/hw/floppy.c FLOPPY_DIR_DSKCHG).
enum { kDirDoor = 0x80 };
// Tape Drive Register: the media boot-selection bit is the only one defined.
enum { kTdrBootSel = 0x04 };

// Status Register 0 (8272A datasheet).
enum {
  kSr0Drive = 0x03,
  kSr0Head = 0x04,
  kSr0Seek = 0x20,
  kSr0AbnTerm = 0x40,
  kSr0InvCmd = 0x80,
  // Set after a reset and after a successful SENSE INTERRUPT STATUS: the
  // controller's "not ready to report anything" state.
  kSr0RdyChg = 0x40 | 0x80,
};
// Status Register 1.
enum { kSr1Missing = 0x01, kSr1NotWritable = 0x02, kSr1EndCyl = 0x80 };

// Commands, by the low five bits of the code. The upper bits select
// modifiers: multi-track (0x80) for read and write, MFM (0x40), and the skip
// and data-deleted flags the deleted-data variants carry.
enum {
  kCmdReadTrack = 0x02,
  kCmdSpecify = 0x03,
  kCmdSenseDrive = 0x04,
  kCmdWrite = 0x05,
  kCmdRead = 0x06,
  kCmdRecalibrate = 0x07,
  kCmdSenseInterrupt = 0x08,
  kCmdReadId = 0x0a,
  kCmdFormatTrack = 0x0d,
  kCmdSeek = 0x0f,
};
// Command modifier bits.
enum { kCmdMultiTrack = 0x80 };
// The controller's phases.
enum { kPhaseCommand = 1, kPhaseExecution = 2, kPhaseResult = 3 };
// A controller reset leaves one interrupt pending for each of the four
// logical drives; the guest retires them with four SENSE INTERRUPT STATUS
// commands (Intel 82077AA datasheet §7.4 "Drive Polling").
enum { kResetSenseMax = 4 };
// Sector size code 2 = 512 bytes, the only size the media here uses.
enum { kSizeCode512 = 2 };

// What a floppy image's size says about the drive and medium it represents,
// and which CMOS equipment-byte value reports it. The geometry per capacity is
// the standard one for each diskette size and follows seabios
// src/hw/floppy.c FloppyInfo[]; the type codes are the ones CMOS register 0x10
// carries, whose nibbles firmware reads to decide which drives exist.
typedef struct FdcFormat {
  int64_t bytes;
  uint8_t cmos_type;
  int cylinders;
  int heads;
  int sectors;
} FdcFormat;

static const FdcFormat kFdcFormats[] = {
    {163840, 0x6, 40, 1, 8},    // 160K 5.25"
    {184320, 0x7, 40, 1, 9},    // 180K 5.25"
    {327680, 0x8, 40, 2, 8},    // 320K 5.25"
    {368640, 0x1, 40, 2, 9},    // 360K 5.25"
    {737280, 0x3, 80, 2, 9},    // 720K 3.5"
    {1228800, 0x2, 80, 2, 15},  // 1.2M 5.25"
    {1474560, 0x4, 80, 2, 18},  // 1.44M 3.5"
    {2949120, 0x5, 80, 2, 36},  // 2.88M 3.5"
};

// ---- lines and phases -------------------------------------------------------

static void RaiseIrq(FdcDevice* d) {
  // The controller holds its interrupt line up until the guest collects the
  // result with SENSE INTERRUPT STATUS (8272A datasheet).
  if (!(d->sra & kSraIntPend)) {
    d->sra |= kSraIntPend;
    d->irq = 1;
    if (DebugOn(kDbgMark)) DebugMark("fdc-irq", 1, d->status0);
    if (d->set_irq) d->set_irq(d->irq_ctx, kFdcIrqLine, 1);
  }
  // A new interrupt supersedes any reset interrupts still owed.
  d->reset_sense = 0;
}

static void LowerIrq(FdcDevice* d) {
  d->status0 = 0;
  if (d->sra & kSraIntPend) {
    d->sra &= (uint8_t)~kSraIntPend;
    d->irq = 0;
    if (DebugOn(kDbgMark)) DebugMark("fdc-irq", 0, 0);
    if (d->set_irq) d->set_irq(d->irq_ctx, kFdcIrqLine, 0);
  }
}

static void EnterCommandPhase(FdcDevice* d) {
  d->phase = kPhaseCommand;
  d->cmd_cursor = 0;
  d->cmd_remaining = 0;
  d->msr &= (uint8_t)~(kMsrCmdBusy | kMsrDio);
  d->msr |= kMsrRqm;
}

static void EnterResultPhase(FdcDevice* d, int count) {
  d->phase = kPhaseResult;
  d->resp_cursor = 0;
  d->resp_len = count;
  d->msr |= (uint8_t)(kMsrCmdBusy | kMsrRqm | kMsrDio);
}

// ---- geometry ---------------------------------------------------------------

static int64_t ChsToLba(const FdcDrive* drv, int cyl, int head, int sect) {
  return ((int64_t)cyl * drv->heads + head) * drv->sectors + (sect - 1);
}

// Moves the head, returning 0 when it was already there, 1 when the track
// changed, 2 when the address is off the medium and 3 when the sector is.
// A step pulse that finds a medium clears the disk-change latch, and only a
// step does: the latch is set when a step finds no medium (8272A datasheet,
// Disk Change).
static int SeekDrive(FdcDevice* d, FdcDrive* drv, int head, int track, int sect) {
  (void)d;
  if (track < 0 || track >= drv->cylinders || (head != 0 && drv->heads < 2)) return 2;
  if (sect < 1 || sect > drv->sectors) return 3;
  int result = 0;
  int64_t cur = ChsToLba(drv, drv->track, drv->head, drv->sector);
  int64_t want = ChsToLba(drv, track, head, sect);
  if (cur != want) {
    if (drv->track != track) {
      if (drv->image) drv->media_changed = 0;
      result = 1;
    }
    drv->track = track;
    drv->head = head;
    drv->sector = sect;
  }
  if (!drv->image) result = 2;
  return result;
}

// Where a completed transfer leaves the head: one sector per sector actually
// moved, which is what the data phase covered rather than what the command
// asked for — it can end at the DMA controller's terminal count first (see
// DoReadWrite). With the multi-track bit set the second head follows before
// the cylinder advances (8272A datasheet, "Read Data"/"Write Data" operation).
static void AdvanceHead(FdcDevice* d, FdcDrive* drv, int sectors) {
  for (int i = 0; i < sectors; i++) {
    int track = drv->track, head = drv->head, sect = drv->sector;
    if (sect >= drv->sectors || sect == (int)d->eot) {
      sect = 1;
      if (d->cmd_flags & kCmdMultiTrack) {
        if (head == 0 && drv->heads == 2) {
          head = 1;
        } else {
          head = 0;
          track++;
          d->status0 |= kSr0Seek;
        }
      } else {
        track++;
        d->status0 |= kSr0Seek;
      }
    } else {
      sect++;
    }
    if (SeekDrive(d, drv, head, track, sect) >= 2) break;
  }
}

// The seven result bytes every data-transfer command ends with: the three
// status registers, then where the head ended up and the sector size code
// (8272A datasheet, "Result Phase").
static void EndReadWrite(FdcDevice* d, uint8_t status0, uint8_t status1, uint8_t status2) {
  FdcDrive* drv = &d->drives[d->curr_drive];
  d->status0 &= (uint8_t)~(kSr0Drive | kSr0Head);
  d->status0 |= (uint8_t)d->curr_drive;
  if (drv->head) d->status0 |= kSr0Head;
  d->status0 |= status0;
  d->status1 = status1;
  d->status2 = status2;
  d->msr |= (uint8_t)(kMsrRqm | kMsrDio);
  d->msr &= (uint8_t)~kMsrNonDma;
  d->resp[0] = d->status0;
  d->resp[1] = d->status1;
  d->resp[2] = d->status2;
  d->resp[3] = (uint8_t)drv->track;
  d->resp[4] = (uint8_t)drv->head;
  d->resp[5] = (uint8_t)drv->sector;
  d->resp[6] = kSizeCode512;
  EnterResultPhase(d, 7);
  if (DebugOn(kDbgMark)) DebugMark("fdc-res", d->resp_len, d->status0);
  RaiseIrq(d);
}

// A command the controller does not implement answers with the invalid-command
// bit set and no interrupt (8272A datasheet, Status Register 0 IC).
static void DoInvalid(FdcDevice* d) {
  d->status0 = kSr0InvCmd;
  d->resp[0] = d->status0;
  EnterResultPhase(d, 1);
}

// ---- commands ---------------------------------------------------------------

// How many parameter bytes a command expects, keyed by the DECODED command:
// the code the guest sends carries modifier bits (0xE6 is READ with MFM and
// multi-track set), so the raw byte never matches a bare command code. This
// is each command's "Command Phase" byte count from the 8272A datasheet.
static int ArgCount(int command) {
  switch (command) {
    case kCmdRead:
    case kCmdWrite:
      return 8;
    case kCmdSeek:
    case kCmdSpecify:
      return 2;
    case kCmdRecalibrate:
    case kCmdReadId:
    case kCmdSenseDrive:
      return 1;
    case kCmdFormatTrack:
      return 5;
    default:
      return 0;
  }
}

// The command code with its modifier bits stripped, or -1 when the controller
// does not implement it. The masks are the ones QEMU's fdc.c command table
// uses: read and write ignore everything but their low bits, format and read
// id ignore the MFM bit, and the rest are exact.
static int DecodeCommand(uint8_t code) {
  if ((code & 0x1f) == kCmdRead) return kCmdRead;
  if ((code & 0x3f) == kCmdWrite) return kCmdWrite;
  if (code == kCmdSeek) return kCmdSeek;
  if (code == kCmdSenseInterrupt) return kCmdSenseInterrupt;
  if (code == kCmdRecalibrate) return kCmdRecalibrate;
  if ((code & 0xbf) == kCmdFormatTrack) return kCmdFormatTrack;
  if ((code & 0xbf) == kCmdReadId) return kCmdReadId;
  if (code == kCmdSpecify) return kCmdSpecify;
  if (code == kCmdSenseDrive) return kCmdSenseDrive;
  return -1;
}

static void DoReadWrite(FdcDevice* d, int is_write) {
  int drive = d->cmd_args[0] & kDorSelect;
  d->curr_drive = drive & 1;
  FdcDrive* drv = &d->drives[d->curr_drive];
  int cyl = d->cmd_args[1];
  int head = d->cmd_args[2];
  int sect = d->cmd_args[3];
  int size_code = d->cmd_args[4];
  int eot = d->cmd_args[5];
  int dtl = d->cmd_args[7];
  // a = R << 8 | EOT, b = cylinder << 16 | head << 8 | size code: what the
  // guest actually asked the head to do.
  if (DebugOn(kDbgMark)) DebugMark("fdc-rw", (sect << 8) | eot, (cyl << 16) | (head << 8) | size_code);

  int seek = SeekDrive(d, drv, head, cyl, sect);
  if (seek == 2 || seek == 3) {
    // Off the medium: the operation terminates abnormally and reports where
    // the head still is (8272A datasheet, Status Register 0/1).
    d->status0 |= seek == 2 ? kSr0AbnTerm : (kSr0AbnTerm | kSr0Seek);
    d->resp[3] = (uint8_t)cyl;
    d->resp[4] = (uint8_t)head;
    d->resp[5] = (uint8_t)sect;
    EndReadWrite(d, d->status0, seek == 2 ? kSr1EndCyl : kSr1Missing, 0);
    return;
  }
  if (seek == 1) d->status0 |= kSr0Seek;

  int sect_size = 128 << (size_code > 7 ? 7 : size_code);
  int64_t data_length;
  if (sect_size == 128) {
    // With the 128-byte sector size the data length register is the transfer
    // length itself (8272A datasheet, "Read Data").
    data_length = dtl < 128 ? dtl : 128;
  } else {
    // The command spans the sectors from R through EOT; with the multi-track
    // bit set the controller carries on to the second head once it reaches
    // EOT, so the span covers both sides: 2*EOT - R + 1 sectors (8272A
    // datasheet, Read Data command — the MT, EOT and R parameters).
    data_length = (d->cmd_flags & kCmdMultiTrack) ? (int64_t)(2 * eot - sect + 1) * sect_size
                                                  : (int64_t)(eot - sect + 1) * sect_size;
    if (data_length <= 0) {
      EndReadWrite(d, kSr0AbnTerm, kSr1Missing, 0);
      return;
    }
  }
  if (data_length > kFdcMaxTransfer) data_length = kFdcMaxTransfer;
  d->eot = (uint8_t)eot;

  if (is_write && drv->read_only) {
    EndReadWrite(d, (uint8_t)(kSr0AbnTerm | kSr0Seek), kSr1NotWritable, 0);
    return;
  }

  int64_t offset = ChsToLba(drv, cyl, head, sect) * sect_size;
  // Execution phase: the data register is unavailable while the transfer runs,
  // which the main status register's RQM bit reports (8272A datasheet).
  d->msr &= (uint8_t)~kMsrRqm;
  int want = (int)data_length;
  int moved = 0;
  if (is_write) {
    // The bytes come out of guest memory through the DMA channel and land in
    // the image.
    moved = d->dma ? d->dma(d->dma_ctx, d->buf, want) : 0;
    if (moved > 0 && drv->image) HostFileWriteAt(drv->image, offset, d->buf, (size_t)moved);
  } else {
    if (drv->image) HostFileReadAt(drv->image, offset, d->buf, (size_t)want);
    moved = d->dma ? d->dma(d->dma_ctx, d->buf, want) : 0;
  }
  if (moved <= 0) {
    // Nothing moved at all: the channel was masked or the controller
    // disabled, so the command cannot complete and reports an abnormal
    // termination.
    if (DebugOn(kDbgMark)) DebugMark("fdc-xfer", want, moved);
    EndReadWrite(d, kSr0AbnTerm, 0, 0);
    return;
  }
  // The data phase ends at the DMA controller's terminal count as well as at
  // the command's own length, and that is a normal end, not a failure: the
  // 8237 asserts TC when its count runs out and the controller takes it as the
  // end of the transfer (Intel 8237A datasheet, Terminal Count; 8272A
  // datasheet, the data phase of Read/Write Data). Only a transfer that moved
  // nothing at all means the command could not run.
  if (DebugOn(kDbgMark)) DebugMark("fdc-xfer", want, moved);
  AdvanceHead(d, drv, moved / sect_size);
  EndReadWrite(d, 0, 0, 0);
}

static void DoSeek(FdcDevice* d) {
  d->curr_drive = d->cmd_args[0] & kDorSelect;
  FdcDrive* drv = &d->drives[d->curr_drive];
  SeekDrive(d, drv, drv->head, d->cmd_args[1], drv->sector);
  // SEEK has no result bytes; its completion is the interrupt plus the status
  // the following SENSE INTERRUPT STATUS reports (8272A datasheet).
  EnterCommandPhase(d);
  d->status0 |= kSr0Seek;
  RaiseIrq(d);
}

static void DoRecalibrate(FdcDevice* d) {
  d->curr_drive = d->cmd_args[0] & kDorSelect;
  FdcDrive* drv = &d->drives[d->curr_drive];
  SeekDrive(d, drv, 0, 0, 1);
  EnterCommandPhase(d);
  d->status0 |= kSr0Seek;
  RaiseIrq(d);
}

static void DoSenseInterrupt(FdcDevice* d) {
  FdcDrive* drv = &d->drives[d->curr_drive];
  uint8_t status0;
  if (d->reset_sense > 0) {
    // The reset interrupts are reported one per logical drive, in order, with
    // the not-ready-to-seek bit set (Intel 82077AA datasheet §7.4).
    int drive = kResetSenseMax - d->reset_sense;
    d->reset_sense--;
    status0 = (uint8_t)(kSr0RdyChg | drive);
  } else if (d->sra & kSraIntPend) {
    status0 = (uint8_t)((d->status0 & ~(kSr0Head | kSr0Drive)) | d->curr_drive);
  } else {
    DoInvalid(d);
    return;
  }
  d->resp[0] = status0;
  d->resp[1] = (uint8_t)drv->track;
  EnterResultPhase(d, 2);
  LowerIrq(d);
  d->status0 = kSr0RdyChg;
}

static void DoReadId(FdcDevice* d) {
  d->curr_drive = d->cmd_args[0] & kDorSelect;
  FdcDrive* drv = &d->drives[d->curr_drive];
  drv->head = (d->cmd_args[0] >> 2) & 1;
  if (drv->sectors > 0) drv->sector = (drv->sector % drv->sectors) + 1;
  // The seven result bytes carry the ID field the head is over.
  EndReadWrite(d, 0, 0, 0);
}

static void DoSenseDriveStatus(FdcDevice* d) {
  d->curr_drive = d->cmd_args[0] & kDorSelect;
  FdcDrive* drv = &d->drives[d->curr_drive];
  drv->head = (d->cmd_args[0] >> 2) & 1;
  // The one result byte is Status Register 3: write-protect, track 0, head,
  // drive select, and the two bits that always read back as one (8272A
  // datasheet).
  d->resp[0] = (uint8_t)((drv->read_only ? 0x40 : 0) | (drv->track == 0 ? 0x10 : 0) |
                         (drv->head << 2) | d->curr_drive | 0x28);
  EnterResultPhase(d, 1);
}

static void DoFormatTrack(FdcDevice* d) {
  d->curr_drive = d->cmd_args[0] & kDorSelect;
  FdcDrive* drv = &d->drives[d->curr_drive];
  int nsect = d->cmd_args[2];
  uint8_t fill = d->cmd_args[4];
  if (drv->read_only) {
    EndReadWrite(d, (uint8_t)(kSr0AbnTerm | kSr0Seek), kSr1NotWritable, 0);
    return;
  }
  // The channel carries the four-byte descriptor (C, H, R, N) of every
  // sector the guest wants laid down; the sector data itself is the fill byte
  // (8272A datasheet, "Format Track"). The descriptor is read for its length
  // and consumed, the way the controller's DMA request does.
  int descriptor_bytes = nsect * 4;
  int moved = 0;
  if (descriptor_bytes > 0 && descriptor_bytes <= (int)sizeof(d->buf) && d->dma)
    moved = d->dma(d->dma_ctx, d->buf, descriptor_bytes);
  if (drv->image && moved == descriptor_bytes) {
    uint8_t filler[kFdcSectorSize];
    memset(filler, fill, sizeof(filler));
    int64_t lba = ChsToLba(drv, drv->track, drv->head, 1);
    for (int i = 0; i < nsect && i < drv->sectors; i++)
      HostFileWriteAt(drv->image, (lba + i) * kFdcSectorSize, filler, sizeof(filler));
  }
  AdvanceHead(d, drv, 1);
  EndReadWrite(d, 0, 0, 0);
}

static void ExecuteCommand(FdcDevice* d) {
  d->phase = kPhaseExecution;
  int command = DecodeCommand(d->cmd_code);
  // a = the raw code the guest sent (decimal), b = what it decoded to.
  if (DebugOn(kDbgMark)) DebugMark("fdc-cmd", d->cmd_code, command);
  switch (command) {
    case kCmdRead:
      DoReadWrite(d, 0);
      break;
    case kCmdWrite:
      DoReadWrite(d, 1);
      break;
    case kCmdSeek:
      DoSeek(d);
      break;
    case kCmdRecalibrate:
      DoRecalibrate(d);
      break;
    case kCmdSenseInterrupt:
      DoSenseInterrupt(d);
      break;
    case kCmdReadId:
      DoReadId(d);
      break;
    case kCmdSenseDrive:
      DoSenseDriveStatus(d);
      break;
    case kCmdFormatTrack:
      DoFormatTrack(d);
      break;
    case kCmdSpecify:
      // SPECIFY programs the step rate, head load and unload times and the DMA
      // bit. This model has no transfer timing, so there is nothing for the
      // parameters to change and the command completes with no result and no
      // interrupt (8272A datasheet, Specify command).
      EnterCommandPhase(d);
      break;
    default:
      DoInvalid(d);
      break;
  }
}

// ---- registers --------------------------------------------------------------

static void WriteFifo(FdcDevice* d, uint8_t v) {
  if (!(d->dor & kDorNReset)) return;  // the controller is held in reset
  if (!(d->msr & kMsrRqm) || (d->msr & kMsrDio)) return;
  if (d->phase != kPhaseCommand) return;
  if (d->cmd_remaining == 0) {
    // The first byte of a command: the code, and how many parameters follow.
    d->cmd_code = v;
    d->cmd_cursor = 0;
    d->cmd_flags = 0;
    int decoded = DecodeCommand(v);
    d->cmd_remaining = decoded < 0 ? 0 : ArgCount(decoded);
    if (decoded == kCmdRead || decoded == kCmdWrite) {
      if (v & kCmdMultiTrack) d->cmd_flags |= kCmdMultiTrack;
    }
    d->msr |= kMsrCmdBusy;
  } else {
    if (d->cmd_cursor < (int)sizeof(d->cmd_args)) d->cmd_args[d->cmd_cursor] = v;
    d->cmd_cursor++;
    d->cmd_remaining--;
  }
  if (d->cmd_remaining == 0) ExecuteCommand(d);
}

static uint64_t ReadFifo(FdcDevice* d) {
  if (!(d->msr & kMsrRqm) || !(d->msr & kMsrDio)) return 0;
  if (d->phase != kPhaseResult) return 0;
  if (d->resp_cursor >= d->resp_len) return 0;
  uint8_t v = d->resp[d->resp_cursor++];
  if (d->resp_cursor == d->resp_len) {
    // The last result byte ends the command: the controller returns to the
    // command phase and drops its interrupt line (8272A datasheet, "Result
    // Phase").
    d->msr &= (uint8_t)~kMsrRqm;
    EnterCommandPhase(d);
    LowerIrq(d);
  }
  return v;
}

// The reset state of every register and of the head position, without the
// interrupt a guest-commanded controller reset raises. A machine reset is
// this (a power cycle is not a DOR reset transition).
static void ControllerStateReset(FdcDevice* d) {
  d->sra = 0;
  d->srb = kSrbReset;
  d->dor = (uint8_t)(kDorNReset | kDorDmaEn);
  d->tdr = 0;
  d->msr = kMsrRqm;
  d->status0 = 0;
  d->status1 = 0;
  d->status2 = 0;
  d->cmd_code = 0;
  d->cmd_flags = 0;
  d->cmd_cursor = 0;
  d->cmd_remaining = 0;
  d->phase = kPhaseCommand;
  d->resp_cursor = 0;
  d->resp_len = 0;
  d->curr_drive = 0;
  d->reset_sense = 0;
  d->eot = 0;
  for (int i = 0; i < kFdcDrives; i++) {
    d->drives[i].track = 0;
    d->drives[i].head = 0;
    d->drives[i].sector = 1;
  }
  if (d->irq) {
    d->irq = 0;
    if (d->set_irq) d->set_irq(d->irq_ctx, kFdcIrqLine, 0);
  }
}

// A guest-commanded controller reset: the DOR's reset line went from 0 to 1.
// It raises the interrupt the guest collects with four SENSE INTERRUPT STATUS
// commands (Intel 82077AA datasheet §7.4).
static void ControllerReset(FdcDevice* d) {
  ControllerStateReset(d);
  d->status0 = kSr0RdyChg;
  RaiseIrq(d);
  d->reset_sense = kResetSenseMax;
}

static void WriteDor(FdcDevice* d, uint8_t v) {
  // Status Register B mirrors the motor and drive-select lines the DOR drives
  // (8272A datasheet; v86 src/floppy.js write_reg_dor).
  d->srb &= (uint8_t)~(kSrbMotor0 | kSrbMotor1 | kSrbDrive0);
  if (v & kDorMotor0) d->srb |= kSrbMotor0;
  if (v & kDorMotor1) d->srb |= kSrbMotor1;
  if (v & 0x01) d->srb |= kSrbDrive0;
  if (!(d->dor & kDorNReset) && (v & kDorNReset)) ControllerReset(d);
  // Only the low select bit picks a drive: a PC has A: and B:, and the second
  // bit would name drive 2 which does not exist (v86 src/floppy.js does the
  // same).
  d->curr_drive = v & 0x01;
  d->dor = v;
}

static uint64_t FdcRead(void* dev, uint64_t addr, int size) {
  (void)size;
  FdcDevice* d = (FdcDevice*)dev;
  switch ((int)(addr - kFdcBase)) {
    case kRegSra:
      return d->sra;
    case kRegSrb:
      return d->srb;
    case kRegDor:
      // The drive-select field reads back as the drive actually selected.
      return (uint8_t)((d->dor & ~kDorSelect) | d->curr_drive);
    case kRegTdr:
      return d->tdr;
    case kRegMsr:
      return d->msr;
    case kRegFifo:
      return ReadFifo(d);
    case kRegDir:
      return d->drives[d->curr_drive].media_changed ? kDirDoor : 0;
    default:
      return 0xff;
  }
}

static void FdcWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  FdcDevice* d = (FdcDevice*)dev;
  uint8_t v = (uint8_t)val;
  switch ((int)(addr - kFdcBase)) {
    case kRegDor:
      WriteDor(d, v);
      break;
    case kRegTdr:
      if (d->dor & kDorNReset) d->tdr = (uint8_t)(v & kTdrBootSel);
      break;
    case kRegFifo:
      WriteFifo(d, v);
      break;
    case kRegDir:
      // 0x3F7 also carries the configuration control register, which selects
      // the data rate. This model has no transfer timing, so the value has
      // nothing to change — the data itself is the same at any rate.
      break;
    default:
      break;
  }
}

static const DeviceOps kFdcOps = {"fdc", FdcRead, FdcWrite};

// ---- public interface -------------------------------------------------------

void FdcInit(FdcDevice* d) {
  memset(d, 0, sizeof(*d));
  ControllerStateReset(d);
}

void FdcReset(FdcDevice* d) { ControllerStateReset(d); }

int FdcAttach(FdcDevice* d, int drive, const char* path) {
  if (drive < 0 || drive >= kFdcDrives) return -1;
  HostFile* f = HostFileOpenReadWrite(path);
  if (!f) {
    LogError("cannot open floppy image '%s'", path);
    return -1;
  }
  int64_t size = HostFileSize(f);
  const FdcFormat* fmt = NULL;
  for (size_t i = 0; i < sizeof(kFdcFormats) / sizeof(kFdcFormats[0]); i++) {
    if (kFdcFormats[i].bytes == size) {
      fmt = &kFdcFormats[i];
      break;
    }
  }
  if (!fmt) {
    HostFileClose(f);
    LogError("floppy image '%s' is not a known format (%lld bytes)", path, (long long)size);
    return -1;
  }
  FdcDrive* drv = &d->drives[drive];
  drv->image = f;
  drv->bytes = size;
  drv->cylinders = fmt->cylinders;
  drv->heads = fmt->heads;
  drv->sectors = fmt->sectors;
  drv->cmos_type = fmt->cmos_type;
  drv->read_only = 0;
  drv->media_changed = 1;
  drv->track = 0;
  drv->head = 0;
  drv->sector = 1;
  LogInfo("floppy %c: '%s' %lld bytes, %d cyl / %d heads / %d sectors (cmos type %d)", 'A' + drive,
          path, (long long)size, fmt->cylinders, fmt->heads, fmt->sectors, fmt->cmos_type);
  return 0;
}

void FdcRegister(Bus* io, FdcDevice* d) {
  BusAddRegion(io, kFdcBase, kFdcRegCount, &kFdcOps, d);
  BusAddRegion(io, kFdcDirPort, 1, &kFdcOps, d);
}

void FdcSetIrqSink(FdcDevice* d, void (*set_irq)(void* ctx, int line, int level), void* ctx) {
  d->set_irq = set_irq;
  d->irq_ctx = ctx;
}

void FdcSetDmaSink(FdcDevice* d, int (*dma)(void* ctx, uint8_t* buf, int len), void* ctx) {
  d->dma = dma;
  d->dma_ctx = ctx;
}

uint8_t FdcCmosType(const FdcDevice* d, int drive) {
  if (drive < 0 || drive >= kFdcDrives) return 0;
  return d->drives[drive].cmos_type;
}

void FdcDestroy(FdcDevice* d) {
  for (int i = 0; i < kFdcDrives; i++) {
    if (d->drives[i].image) {
      HostFileClose(d->drives[i].image);
      d->drives[i].image = NULL;
    }
  }
}
