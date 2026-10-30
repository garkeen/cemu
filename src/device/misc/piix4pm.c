#include "device/misc/piix4pm.h"

#include <string.h>

#include "host/host.h"

// PCI configuration offsets and this function's identity (PCI spec §6.1;
// seabios src/hw/pci_ids.h PCI_DEVICE_ID_INTEL_82371AB_3 = 0x7113,
// src/fw/dev-piix.h for PIIX_PMBASE and PIIX_PMREGMISC).
enum {
  kCfgVendor = 0x00,
  kCfgDevice = 0x02,
  kCfgRevision = 0x08,
  kCfgProgIf = 0x09,
  kCfgSubClass = 0x0a,
  kCfgClass = 0x0b,
  kCfgIntPin = 0x3d,
  kCfgPmBase = 0x40,
  kCfgPmRegMisc = 0x80,
  // SMBus: SMB_BASE (the 64-byte window's address, bit 0 = I/O-space indicator)
  // and the device-resources byte whose bit 0 gates its decode (hw/acpi/piix4.c
  // smbus_io_space_update reads both).
  kCfgSmbBase = 0x90,
  kCfgDeviceResources = 0xd2,
};
enum {
  kVendorIntel = 0x8086,
  kDevicePiix4Pm = 0x7113,
  // QEMU's -machine pc reports this function at revision 3.
  kRevision = 3,
  kProgIf = 0,
  // Class 0x0680: bridge, other — what `info pci` prints as "Bridge".
  kSubClassOther = 0x80,
  kClassBridge = 0x06,
  kIntPinA = 1,
};

// The PM register block. Its size and the offsets inside it come from what
// firmware programs and reads: seabios src/fw/pciinit.c piix4_pm_config_setup
// writes the block base at PIIX_PMBASE with the I/O-space indicator in bit 0 and
// turns the block on with PIIX_PMREGMISC bit 0 ("enable PM io space"), and
// src/hw/timer.c pmtimer_setup(acpi_pm_base + 0x08) reads the timer there while
// src/fw/pciinit.c takes PM1a control at +0x04.
enum {
  kPmBlockSize = 32,
  kPmTimerOffset = 0x08,
  kPmIoSpaceEnable = 0x01,  // PIIX_PMREGMISC bit 0
  kPmBaseIoIndicator = 0x01,
};

// What the SMBus configuration image holds when the machine starts: the window
// at 0xb100 with the I/O indicator, and the device-resources byte that turns it
// on (hw/i386/pc_piix.c passes smb_io_base = 0xb100 to this function and
// hw/acpi/piix4.c writes 0xb101 and 0xD2 = 0x09 before the guest runs).
enum {
  kSmbusDefaultBase = 0xb100,
  kSmbusBaseIoIndicator = 0x01,
  kSmbusDeviceResourcesOn = 0x09,
};

// The timer's rate: 3.579545 MHz, the 32.768 kHz watch crystal times 109.2 —
// the frequency ACPI defines for the PM timer and the one seabios src/hw/timer.c
// PMTIMER_HZ states, from which it derives every microsecond the firmware waits.
static const uint64_t kPmTimerHz = 3579545;
static const uint64_t kMicroPerSec = 1000000;

static uint32_t PmTicks(void) {
  return (uint32_t)((uint64_t)HostTimerNow() * kPmTimerHz / kMicroPerSec);
}

// The fixed-event bits the PM1a block implements (ACPI 4.8): the timer
// overflow, the global-lock release, the power button, the RTC alarm and the
// wake status. PM1_STS is write-one-to-clear; PM1_EN has the same bit
// positions and gates those events onto the SCI.
enum {
  kPm1StsTimer = 0x0001,
  kPm1StsGlobalLock = 0x0020,
  kPm1StsPowerButton = 0x0100,
  kPm1StsRtc = 0x0400,
  kPm1StsWake = 0x8000,
  kPm1StatusMask = kPm1StsTimer | kPm1StsGlobalLock | kPm1StsPowerButton |
                   kPm1StsRtc | kPm1StsWake,
  kPm1EnMask = kPm1StsTimer | kPm1StsGlobalLock | kPm1StsPowerButton | kPm1StsRtc,
  // PM1_CNT: SCI_EN, BM_RLD, GBL_RLS, SLP_TYP (bits 12:10) and SLP_EN (13).
  kPm1CntSciEn = 0x0001,
  kPm1CntBmRld = 0x0002,
  kPm1CntGlobalRelease = 0x0004,
  kPm1CntSleepType = 0x1c00,
  kPm1CntSleepEnable = 0x2000,
  kPm1SleepTypeShift = 10,
  // S5 is the soft-off state; S3/S4 need a wake path this machine does not
  // have (简化登记 D19).
  kPm1SleepS5 = 5,
  // The PIIX4 PM function's SCI is the machine's IRQ9 (QEMU piix4.c's
  // sci_int default and the value SeaBIOS's FADT names).
  kSciIrq = 9,
};

static void PmUpdateSci(Piix4PmDevice* d) {
  int level = (d->pm1_cnt & kPm1CntSciEn) && (d->pm1_sts & d->pm1_en) != 0;
  if (d->set_sci) d->set_sci(d->sci_ctx, kSciIrq, level);
}

static uint8_t PmReadByte(const Piix4PmDevice* d, uint64_t off) {
  switch (off) {
    case 0:
    case 1:
      return (uint8_t)(d->pm1_sts >> (8 * off));
    case 2:
    case 3:
      return (uint8_t)(d->pm1_en >> (8 * (off - 2)));
    case 4:
    case 5:
      return (uint8_t)(d->pm1_cnt >> (8 * (off - 4)));
    default:
      break;
  }
  if (off >= kPmTimerOffset && off < kPmTimerOffset + 4)
    return (uint8_t)(PmTicks() >> (8 * (off - kPmTimerOffset)));
  return 0;
}

static uint64_t PmRead(void* dev, uint64_t addr, int size) {
  Piix4PmDevice* d = (Piix4PmDevice*)dev;
  const uint64_t off = addr - (uint64_t)d->base;
  uint64_t v = 0;
  for (int i = 0; i < size; i++)
    v |= (uint64_t)PmReadByte(d, off + (uint64_t)i) << (8 * i);
  return v;
}

// A PM1_CNT write's action bits are edges: GBL_RLS sets GBL_STS and does not
// stay set, and SLP_EN asks the board to enter the named sleep state. SCI_EN
// gates every fixed event onto the SCI (ACPI 4.8; QEMU hw/acpi's
// acpi_pm1_cnt_write/acpi_update_sci).
static void PmWriteControl(Piix4PmDevice* d, uint16_t v) {
  if (v & kPm1CntGlobalRelease) d->pm1_sts |= kPm1StsGlobalLock;
  d->pm1_cnt = (uint16_t)(v & ~kPm1CntGlobalRelease);
  if (d->pm1_cnt & kPm1CntSleepEnable) {
    const int type = (d->pm1_cnt & kPm1CntSleepType) >> kPm1SleepTypeShift;
    if (type == kPm1SleepS5 && d->request_shutdown) d->request_shutdown(d->shutdown_ctx);
  }
  PmUpdateSci(d);
}

static void PmWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  Piix4PmDevice* d = (Piix4PmDevice*)dev;
  const uint64_t off = addr - (uint64_t)d->base;
  for (int i = 0; i < size; i++) {
    const uint8_t b = (uint8_t)(val >> (8 * i));
    const uint64_t o = off + (uint64_t)i;
    const uint16_t v = (uint16_t)(b << (8 * (o & 1)));
    if (o == 0 || o == 1) {
      // Write one to clear a fixed-event bit (ACPI 4.8 PM1_STS).
      d->pm1_sts &= (uint16_t)~(v & kPm1StatusMask);
      PmUpdateSci(d);
    } else if (o == 2 || o == 3) {
      d->pm1_en = (uint16_t)((d->pm1_en & ~(uint16_t)(0xffu << (8 * (o & 1)))) |
                             (v & kPm1EnMask));
      PmUpdateSci(d);
    } else if (o == 4 || o == 5) {
      const uint16_t merged =
          (uint16_t)((d->pm1_cnt & ~(uint16_t)(0xffu << (8 * (o & 1)))) | v);
      PmWriteControl(d, merged);
    }
  }
}

// ---------------------------------------------------------------------------
// SMBus host controller. The register file, the protocol dispatch and the two
// byte-by-byte quirks are hw/i2c/pm_smbus.c's; the bus under it is
// hw/i2c/core.c, the device side hw/i2c/smbus_slave.c, and the window's address
// (configuration 0x90/0xD2) hw/acpi/piix4.c.
// ---------------------------------------------------------------------------

// Register offsets inside the 64-byte window (the PIIX4's SMBus host controller
// map; hw/i2c/pm_smbus.c's #defines carry the same numbers).
enum {
  kSmbHstSts = 0x00,
  kSmbHstCnt = 0x02,
  kSmbHstCmd = 0x03,
  kSmbHstAdd = 0x04,
  kSmbHstDat0 = 0x05,
  kSmbHstDat1 = 0x06,
  kSmbBlkDat = 0x07,
  kSmbAuxCtl = 0x0d,
};

// HST_STS bits (pm_smbus.c's STS_*).
enum {
  kStsHostBusy = 1u << 0,
  kStsIntr = 1u << 1,
  kStsDevErr = 1u << 2,
  kStsBusErr = 1u << 3,
  kStsFailed = 1u << 4,
  kStsSmbAlert = 1u << 5,
  kStsInUse = 1u << 6,
  kStsByteDone = 1u << 7,
};

// HST_CNT bits, and the protocol field (bits 4:2) they carry.
enum {
  kCtlIntrEn = 1u << 0,
  kCtlKill = 1u << 1,
  kCtlLastByte = 1u << 5,
  kCtlStart = 1u << 6,
  kCtlPecEn = 1u << 7,
  kCtlReturnMask = 0x1f,  // what a read of HST_CNT keeps: START always reads 0
  kCtlProtocolShift = 2,
  kCtlProtocolMask = 7,
};
enum {
  kProtQuick = 0,
  kProtByte = 1,
  kProtByteData = 2,
  kProtWordData = 3,
  // 4 is the process call and 7 is reserved: no device on this machine
  // implements either, so both report a device error (pm_smbus.c's default).
  kProtBlockData = 5,
  kProtI2cBlockRead = 6,
};

// HST_AUXCTL bits.
enum { kAuxPec = 1u << 0, kAuxBlk = 1u << 1, kAuxMask = 0x03 };

// The I2C bus events a device sees (hw/i2c/i2c.h) and the SMBus protocol state
// it answers them in (hw/i2c/smbus_slave.c's SMBUS_* modes).
enum { kI2cStartSend, kI2cStartRecv, kI2cFinish, kI2cNack };
enum { kSmbusIdle = 0, kSmbusWriteData, kSmbusReadData, kSmbusDone, kSmbusConfused = -1 };
// The general-call address: a start to it selects every device on the bus.
enum { kI2cBroadcastAddress = 0x00 };

// ---- the device side of a transfer (hw/i2c/smbus_slave.c) ----

static void SlaveDoWrite(SmbusSlave* dev) {
  if (dev->WriteData) dev->WriteData(dev->ctx, dev->write_buf, dev->write_len);
}

// A start, stop or NACK on the wire. The mode machine is what turns a write
// phase followed by a repeated start into the command a device acts on: the
// bytes collected so far are that command plus its data.
static int SlaveEvent(SmbusSlave* dev, int event) {
  switch (event) {
    case kI2cStartSend:
      if (dev->mode != kSmbusIdle) {
        dev->mode = kSmbusConfused;
        return 0;
      }
      dev->mode = kSmbusWriteData;
      return 0;
    case kI2cStartRecv:
      if (dev->mode == kSmbusIdle) {
        dev->mode = kSmbusReadData;
        return 0;
      }
      if (dev->mode == kSmbusWriteData && dev->write_len > 0) {
        SlaveDoWrite(dev);
        dev->mode = kSmbusReadData;
        return 0;
      }
      dev->mode = kSmbusConfused;
      return 0;
    case kI2cFinish:
      // A quick command carries no bytes at all; no device on this machine
      // implements one, so it only changes the mode of the device that saw it.
      if (dev->write_len > 0 && dev->mode == kSmbusWriteData) SlaveDoWrite(dev);
      dev->mode = kSmbusIdle;
      dev->write_len = 0;
      return 0;
    case kI2cNack:
      if (dev->mode == kSmbusDone) return 0;
      if (dev->mode != kSmbusReadData) {
        dev->mode = kSmbusConfused;
        return 0;
      }
      dev->mode = kSmbusDone;
      return 0;
    default:
      return -1;
  }
}

static uint8_t SlaveRecv(SmbusSlave* dev) {
  if (dev->mode != kSmbusReadData) {
    dev->mode = kSmbusConfused;
    return 0xff;
  }
  return dev->ReceiveByte(dev->ctx);
}

static int SlaveSend(SmbusSlave* dev, uint8_t data) {
  // A byte outside a write phase, or one more than the protocol allows, is
  // dropped rather than failing the transfer: the wire has already accepted it.
  if (dev->mode != kSmbusWriteData) return 0;
  if (dev->write_len < (int)sizeof(dev->write_buf)) dev->write_buf[dev->write_len++] = data;
  return 0;
}

// ---- the bus (hw/i2c/core.c) ----

static void I2cEndTransfer(Piix4Smbus* s) {
  for (int i = 0; i < s->current_count; i++) SlaveEvent(s->current[i], kI2cFinish);
  s->current_count = 0;
  s->broadcast = 0;
}

// The devices an address selects: the one whose address it is, or every device
// for the general-call address.
static void I2cScanBus(Piix4Smbus* s, uint8_t address) {
  s->current_count = 0;
  for (int i = 0; i < s->slave_count; i++) {
    if (s->slaves[i]->address == address || s->broadcast)
      s->current[s->current_count++] = s->slaves[i];
  }
}

// A start condition. An SMBus transaction opens its read phase without ending
// the write phase, so the bus is not rescanned while a transfer is in progress
// (the repeated start reaches the same device); when no device answers the
// address, the start fails and the caller reports a device error.
static int I2cStartTransfer(Piix4Smbus* s, uint8_t address, int recv) {
  int scanned = 0;
  if (address == kI2cBroadcastAddress) s->broadcast = 1;
  if (s->current_count == 0) {
    I2cScanBus(s, address);
    scanned = 1;
  }
  if (s->current_count == 0) return 1;
  for (int i = 0; i < s->current_count; i++) {
    if (SlaveEvent(s->current[i], recv ? kI2cStartRecv : kI2cStartSend) && !s->broadcast) {
      if (scanned) I2cEndTransfer(s);  // the first start: the transfer never began
      return 1;
    }
  }
  return 0;
}

static int I2cSend(Piix4Smbus* s, uint8_t data) {
  int err = 0;
  for (int i = 0; i < s->current_count; i++) err |= SlaveSend(s->current[i], data);
  return err ? -1 : 0;
}

static uint8_t I2cRecv(Piix4Smbus* s) {
  if (s->current_count == 0 || s->broadcast) return 0xff;
  return SlaveRecv(s->current[0]);
}

static void I2cNack(Piix4Smbus* s) {
  for (int i = 0; i < s->current_count; i++) SlaveEvent(s->current[i], kI2cNack);
}

// ---- the protocols as the host drives them (hw/i2c/smbus_master.c) ----

static int SmbusQuickCommand(Piix4Smbus* s, uint8_t addr, int read) {
  if (I2cStartTransfer(s, addr, read)) return -1;
  I2cEndTransfer(s);
  return 0;
}

static int SmbusReceiveByte(Piix4Smbus* s, uint8_t addr) {
  if (I2cStartTransfer(s, addr, 1)) return -1;
  const uint8_t data = I2cRecv(s);
  I2cNack(s);
  I2cEndTransfer(s);
  return data;
}

static int SmbusSendByte(Piix4Smbus* s, uint8_t addr, uint8_t data) {
  if (I2cStartTransfer(s, addr, 0)) return -1;
  I2cSend(s, data);
  I2cEndTransfer(s);
  return 0;
}

static int SmbusReadByte(Piix4Smbus* s, uint8_t addr, uint8_t command) {
  if (I2cStartTransfer(s, addr, 0)) return -1;
  I2cSend(s, command);
  if (I2cStartTransfer(s, addr, 1)) {
    I2cEndTransfer(s);
    return -1;
  }
  const uint8_t data = I2cRecv(s);
  I2cNack(s);
  I2cEndTransfer(s);
  return data;
}

static int SmbusWriteByte(Piix4Smbus* s, uint8_t addr, uint8_t command, uint8_t data) {
  if (I2cStartTransfer(s, addr, 0)) return -1;
  I2cSend(s, command);
  I2cSend(s, data);
  I2cEndTransfer(s);
  return 0;
}

// The word protocols are little-endian on the wire: the low byte goes out first
// and comes back first.
static int SmbusReadWord(Piix4Smbus* s, uint8_t addr, uint8_t command) {
  uint16_t data;
  if (I2cStartTransfer(s, addr, 0)) return -1;
  I2cSend(s, command);
  if (I2cStartTransfer(s, addr, 1)) {
    I2cEndTransfer(s);
    return -1;
  }
  data = I2cRecv(s);
  data |= (uint16_t)(I2cRecv(s) << 8);
  I2cNack(s);
  I2cEndTransfer(s);
  return data;
}

static int SmbusWriteWord(Piix4Smbus* s, uint8_t addr, uint8_t command, uint16_t data) {
  if (I2cStartTransfer(s, addr, 0)) return -1;
  I2cSend(s, command);
  I2cSend(s, (uint8_t)(data & 0xff));
  I2cSend(s, (uint8_t)(data >> 8));
  I2cEndTransfer(s);
  return 0;
}

// A block read's first byte on the wire is the count (recv_len); a device that
// answers with more than the buffer holds makes the host drop the block.
static int SmbusReadBlock(Piix4Smbus* s, uint8_t addr, uint8_t command, uint8_t* data, int len,
                          int recv_len, int send_cmd) {
  int rlen;
  if (send_cmd) {
    if (I2cStartTransfer(s, addr, 0)) return -1;
    I2cSend(s, command);
  }
  if (I2cStartTransfer(s, addr, 1)) {
    if (send_cmd) I2cEndTransfer(s);
    return -1;
  }
  rlen = recv_len ? I2cRecv(s) : len;
  if (rlen > len) rlen = 0;
  for (int i = 0; i < rlen; i++) data[i] = I2cRecv(s);
  I2cNack(s);
  I2cEndTransfer(s);
  return rlen;
}

static int SmbusWriteBlock(Piix4Smbus* s, uint8_t addr, uint8_t command, const uint8_t* data,
                           int len, int send_len) {
  if (len > kSmbusMaxMsgSize) len = kSmbusMaxMsgSize;
  if (I2cStartTransfer(s, addr, 0)) return -1;
  I2cSend(s, command);
  if (send_len) I2cSend(s, (uint8_t)len);
  for (int i = 0; i < len; i++) I2cSend(s, data[i]);
  I2cEndTransfer(s);
  return 0;
}

// One transaction, started by a write of HST_CNT's START bit. For the reading
// protocols `ret` is the device's answer (a byte, or a word for word-data); a
// negative value is the bus's report that nobody answered.
static void SmbTransaction(Piix4Smbus* s) {
  const uint8_t prot = (uint8_t)((s->ctl >> kCtlProtocolShift) & kCtlProtocolMask);
  const uint8_t read = (uint8_t)(s->addr & 1);
  const uint8_t cmd = s->cmd;
  const uint8_t addr = (uint8_t)(s->addr >> 1);
  int ret;

  // A device error the guest has not cleared yet suppresses this transaction.
  if (s->sts & kStsDevErr) return;

  switch (prot) {
    case kProtQuick:
      if (SmbusQuickCommand(s, addr, read) < 0) goto dev_error;
      break;
    case kProtByte:
      if (read) {
        if ((ret = SmbusReceiveByte(s, addr)) < 0) goto dev_error;
        s->data0 = (uint8_t)ret;
      } else if (SmbusSendByte(s, addr, cmd) < 0) {
        goto dev_error;
      }
      break;
    case kProtByteData:
      if (read) {
        if ((ret = SmbusReadByte(s, addr, cmd)) < 0) goto dev_error;
        s->data0 = (uint8_t)ret;
      } else if (SmbusWriteByte(s, addr, cmd, s->data0) < 0) {
        goto dev_error;
      }
      break;
    case kProtWordData:
      if (read) {
        if ((ret = SmbusReadWord(s, addr, cmd)) < 0) goto dev_error;
        s->data0 = (uint8_t)ret;
        s->data1 = (uint8_t)(ret >> 8);
      } else if (SmbusWriteWord(s, addr, cmd, (uint16_t)((s->data1 << 8) | s->data0)) < 0) {
        goto dev_error;
      }
      break;
    case kProtBlockData:
      if (read) {
        ret = SmbusReadBlock(s, addr, cmd, s->data, kSmbusMaxMsgSize, 1, 1);
        if (ret < 0) goto dev_error;
        s->index = 0;
        s->op_done = 0;
        if (s->auxctl & kAuxBlk) {
          s->sts |= kStsIntr;  // the whole block arrives at once
        } else {
          s->blkdata = s->data[0];  // byte by byte, through HST_BLKDAT
          s->sts |= kStsHostBusy | kStsByteDone;
        }
        s->data0 = (uint8_t)ret;
        return;
      }
      if (s->auxctl & kAuxBlk) {
        // The guest queued the bytes through HST_BLKDAT and HST_DAT0 says how
        // many; a queue and a count that disagree abort the transfer.
        if (s->index != s->data0) {
          s->index = 0;
          goto dev_error;
        }
        s->index = 0;
        if (SmbusWriteBlock(s, addr, cmd, s->data, s->data0, 1) < 0) goto dev_error;
        s->op_done = 1;
        s->sts |= kStsIntr;
        s->sts &= (uint8_t)~kStsHostBusy;
      } else {
        s->op_done = 0;
        s->sts |= kStsHostBusy | kStsByteDone;
        s->data[0] = s->blkdata;
        s->index = 0;
      }
      return;
    case kProtI2cBlockRead:
      // The direction bit is ignored: the write phase carries HST_DAT1 as the
      // command byte and the read phase walks the device on from there
      // (pm_smbus.c's comment on PROT_I2C_BLOCK_READ).
      if (I2cStartTransfer(s, addr, 0)) goto dev_error;
      if (I2cSend(s, s->data1) < 0) {
        I2cEndTransfer(s);
        goto dev_error;
      }
      if (I2cStartTransfer(s, addr, 1)) {
        I2cEndTransfer(s);
        goto dev_error;
      }
      s->in_i2c_block_read = 1;
      s->blkdata = I2cRecv(s);
      s->op_done = 0;
      s->sts |= kStsHostBusy | kStsByteDone;
      return;
    default:
      goto dev_error;
  }
  s->sts |= kStsIntr;
  return;

dev_error:
  s->sts |= kStsDevErr;
}

static void SmbTransactionStart(Piix4Smbus* s) {
  if (s->ctl & kCtlIntrEn) {
    SmbTransaction(s);
    s->start_on_status_read = 0;
  } else {
    // Without INTREN the transaction waits for the guest's HST_STS read:
    // firmware that polls HOST_BUSY before it polls for the result would never
    // see the status move otherwise (pm_smbus.c's AMIBIOS workaround).
    s->sts |= kStsHostBusy;
    s->start_on_status_read = 1;
  }
}

static int SmbByteByByte(const Piix4Smbus* s) {
  if (s->op_done) return 0;
  if (s->in_i2c_block_read) return 1;
  return !(s->auxctl & kAuxBlk);
}

// A write of HST_STS clears the bits written (HOST_BUSY excepted: it is the
// controller's own) and, when BYTE_DONE was among them, moves a byte-by-byte
// transfer on by one byte (pm_smbus.c's smb_ioport_writeb).
static void SmbStatusWrite(Piix4Smbus* s, uint8_t val) {
  const uint8_t clear_byte_done = (uint8_t)(s->sts & val & kStsByteDone);
  s->sts &= (uint8_t)~(val & ~kStsHostBusy);
  if (!clear_byte_done || !SmbByteByByte(s)) return;

  uint8_t read = (uint8_t)(s->addr & 1);
  if (s->in_i2c_block_read) read = 1;
  s->index++;
  if (s->index >= kSmbusMaxMsgSize) s->index = 0;
  if (!read && s->index == s->data0) {
    // The last queued byte: the block goes out with HST_DAT0 as its count.
    const uint8_t prot = (uint8_t)((s->ctl >> kCtlProtocolShift) & kCtlProtocolMask);
    if (prot == kProtI2cBlockRead) {
      s->sts |= kStsDevErr;
      return;
    }
    if (SmbusWriteBlock(s, (uint8_t)(s->addr >> 1), s->cmd, s->data, s->data0, 1) < 0) {
      s->sts |= kStsDevErr;
      return;
    }
    s->op_done = 1;
    s->sts |= kStsIntr;
    s->sts &= (uint8_t)~kStsHostBusy;
    return;
  }
  if (!read) {
    s->data[s->index] = s->blkdata;
    s->sts |= kStsByteDone;
    return;
  }
  if (!(s->ctl & kCtlLastByte)) {
    s->blkdata = s->in_i2c_block_read ? I2cRecv(s) : s->data[s->index];
    s->sts |= kStsByteDone;
    return;
  }
  s->op_done = 1;
  if (s->in_i2c_block_read) {
    s->in_i2c_block_read = 0;
    s->blkdata = I2cRecv(s);
    I2cNack(s);
    I2cEndTransfer(s);
  } else {
    s->blkdata = s->data[s->index];
  }
  s->index = 0;
  s->sts |= kStsIntr;
  s->sts &= (uint8_t)~kStsHostBusy;
}

static uint8_t SmbRead8(Piix4Smbus* s, uint32_t off) {
  switch (off) {
    case kSmbHstSts: {
      const uint8_t val = s->sts;
      if (s->start_on_status_read) {
        // The deferred transaction runs now, and the read answers with the
        // status as it stood before it (pm_smbus.c reads, then executes).
        s->start_on_status_read = 0;
        s->sts &= (uint8_t)~kStsHostBusy;
        SmbTransaction(s);
      }
      return val;
    }
    case kSmbHstCnt:
      return (uint8_t)(s->ctl & kCtlReturnMask);
    case kSmbHstCmd:
      return s->cmd;
    case kSmbHstAdd:
      return s->addr;
    case kSmbHstDat0:
      return s->data0;
    case kSmbHstDat1:
      return s->data1;
    case kSmbBlkDat:
      if ((s->auxctl & kAuxBlk) && !s->in_i2c_block_read) {
        if (s->index >= kSmbusMaxMsgSize) s->index = 0;
        const uint8_t val = s->data[s->index++];
        if (!s->op_done && s->index == s->data0) {
          s->op_done = 1;
          s->index = 0;
          s->sts &= (uint8_t)~kStsHostBusy;
        }
        return val;
      }
      return s->blkdata;
    case kSmbAuxCtl:
      return s->auxctl;
    default:
      return 0;
  }
}

static void SmbWrite8(Piix4Smbus* s, uint32_t off, uint8_t val) {
  switch (off) {
    case kSmbHstSts:
      SmbStatusWrite(s, val);
      break;
    case kSmbHstCnt:
      s->ctl = (uint8_t)(val & ~kCtlStart);  // START never reads back
      if (val & kCtlStart) {
        if (!s->op_done) {
          s->index = 0;
          s->op_done = 1;
          if (s->in_i2c_block_read) {
            s->in_i2c_block_read = 0;
            I2cEndTransfer(s);
          }
        }
        SmbTransactionStart(s);
      }
      if (s->ctl & kCtlKill) {
        s->op_done = 1;
        s->index = 0;
        s->sts |= kStsFailed;
        s->sts &= (uint8_t)~kStsHostBusy;
      }
      break;
    case kSmbHstCmd:
      s->cmd = val;
      break;
    case kSmbHstAdd:
      s->addr = val;
      break;
    case kSmbHstDat0:
      s->data0 = val;
      break;
    case kSmbHstDat1:
      s->data1 = val;
      break;
    case kSmbBlkDat:
      if (s->index >= kSmbusMaxMsgSize) s->index = 0;
      if (s->auxctl & kAuxBlk) s->data[s->index++] = val;
      else s->blkdata = val;
      break;
    case kSmbAuxCtl:
      s->auxctl = (uint8_t)(val & kAuxMask);
      break;
    default:
      break;
  }
}

// The window takes one byte per register (pm_smbus_ops' access size 1); a wider
// guest access lands one register at a time, the split QEMU's access dispatch
// performs for a region that says so.
static uint64_t SmbRead(void* dev, uint64_t addr, int size) {
  Piix4Smbus* s = (Piix4Smbus*)dev;
  const uint64_t off = addr - (uint64_t)s->base;
  uint64_t v = 0;
  for (int i = 0; i < size; i++)
    v |= (uint64_t)SmbRead8(s, (uint32_t)(off + (uint64_t)i)) << (8 * i);
  return v;
}

static void SmbWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  Piix4Smbus* s = (Piix4Smbus*)dev;
  const uint64_t off = addr - (uint64_t)s->base;
  for (int i = 0; i < size; i++)
    SmbWrite8(s, (uint32_t)(off + (uint64_t)i), (uint8_t)(val >> (8 * i)));
}

static const DeviceOps kSmbOps = {"piix4smbus", SmbRead, SmbWrite};

// SMB_BASE names the window (bits 15:6 of configuration offset 0x90) and bit 0
// of offset 0xD2 gates its decode; a write to either register moves it
// (hw/acpi/piix4.c smbus_io_space_update and pm_write_config).
static void SmbMap(Piix4PmDevice* d) {
  const uint8_t* cfg = &d->pci.config[kCfgSmbBase];
  const uint32_t reg = (uint32_t)cfg[0] | ((uint32_t)cfg[1] << 8) | ((uint32_t)cfg[2] << 16) |
                       ((uint32_t)cfg[3] << 24);
  const uint16_t base = (uint16_t)(reg & kSmbusBaseMask);
  // A zero base names no window: enabling it would shadow the DMA controller.
  const int on = (d->pci.config[kCfgDeviceResources] & 1) && base != 0;
  d->smb.base = on ? base : 0;
  if (on)
    BusRelocateRegion(d->io, &kSmbOps, &d->smb, base, kSmbusBlockSize);
  else
    BusRemoveRegion(d->io, &kSmbOps, &d->smb);
}

static const DeviceOps kPmOps = {"piix4pm", PmRead, PmWrite};

// Repoints the block whenever firmware moves PMBASE or gates it with
// PIIX_PMREGMISC bit 0 — the window a PCI device decodes lives where its base
// register says, and firmware can reprogram it (it does so again on the reboot
// path, seabios src/fw/pciinit.c:387).
static void PmMap(Piix4PmDevice* d) {
  uint32_t reg = (uint32_t)d->pci.config[kCfgPmBase] |
                 ((uint32_t)d->pci.config[kCfgPmBase + 1] << 8);
  uint16_t base = (uint16_t)(reg & ~(uint32_t)(kPmBlockSize - 1));
  // A zero base cannot name a window: PMBASE bit 0 selects I/O space and bits
  // 6:1 are reserved, but the address field must be non-zero for the block to
  // decode (otherwise enabling it would shadow the DMA controller at 0x0000).
  int on = (d->pmregmisc & kPmIoSpaceEnable) && (reg & kPmBaseIoIndicator) && base != 0;
  d->base = on ? base : 0;
  if (on)
    BusRelocateRegion(d->io, &kPmOps, d, base, kPmBlockSize);
  else
    BusRemoveRegion(d->io, &kPmOps, d);
}

static void ConfigWritten(void* ctx, uint8_t off, uint8_t val) {
  Piix4PmDevice* d = (Piix4PmDevice*)ctx;
  // Firmware programs the base first and enables the block afterwards (seabios
  // src/fw/pciinit.c piix4_pm_config_setup writes PIIX_PMBASE, then
  // PIIX_PMREGMISC), so either register moves the decode.
  if (off == kCfgPmRegMisc) d->pmregmisc = val;
  if (off == kCfgPmBase || off == kCfgPmBase + 1 || off == kCfgPmRegMisc) PmMap(d);
  // The SMBus window moves the same way, from SMB_BASE or from the gate byte
  // (hw/acpi/piix4.c pm_write_config watches 0x90-0x93 and 0xD2).
  if ((off >= kCfgSmbBase && off <= kCfgSmbBase + 3) || off == kCfgDeviceResources) SmbMap(d);
}

static void Put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)(v >> 8);
}

// What the function answers before any firmware has touched it.
static void FillIdentity(Piix4PmDevice* d) {
  Put16(&d->pci.config[kCfgVendor], kVendorIntel);
  Put16(&d->pci.config[kCfgDevice], kDevicePiix4Pm);
  d->pci.config[kCfgRevision] = kRevision;
  d->pci.config[kCfgProgIf] = kProgIf;
  d->pci.config[kCfgSubClass] = kSubClassOther;
  d->pci.config[kCfgClass] = kClassBridge;
  d->pci.config[kCfgIntPin] = kIntPinA;
}

void Piix4PmInit(Piix4PmDevice* d, uint8_t bus, uint8_t dev) {
  memset(d, 0, sizeof(*d));
  d->pci.bus = bus;
  d->pci.dev = dev;
  // Function 3, where QEMU puts this device (its `info pci` shows 8086:7113 at
  // bus 0 device 1 function 3); firmware reaches it because the ISA bridge at
  // function 0 declares the device multi-function.
  d->pci.func = 3;
  d->pci.written = ConfigWritten;
  d->pci.ctx = d;
  FillIdentity(d);
  // The configuration image QEMU's piix4_pm_realize leaves behind: SMB_BASE at
  // 0xb100 with the I/O indicator and 0xD2 = 0x09 with the window enabled, so
  // the SMBus answers before any firmware has touched it. HST_AUXCTL starts
  // with the 32-byte block mode and no transfer in progress
  // (pm_smbus_init(..., force_aux_blk=true)).
  d->pci.config[kCfgSmbBase] = (uint8_t)((kSmbusDefaultBase | kSmbusBaseIoIndicator) & 0xff);
  d->pci.config[kCfgSmbBase + 1] = (uint8_t)((kSmbusDefaultBase | kSmbusBaseIoIndicator) >> 8);
  d->pci.config[kCfgDeviceResources] = kSmbusDeviceResourcesOn;
  d->smb.auxctl = kAuxBlk;
  d->smb.op_done = 1;
}

// The PM block's decode follows firmware because its base register starts at 0,
// so handing over the address space also publishes the SMBus window: that one
// is already named by the configuration image above and a guest may reach it
// before the firmware has run.
void Piix4PmSetIoBus(Piix4PmDevice* d, Bus* io) {
  d->io = io;
  SmbMap(d);
}

// A reset puts the block back to unprogrammed and off, which is how the chip
// starts: nothing is decoded until firmware names the base and enables it. The
// vendor base register itself returns to its reset value too (0); leaving the
// old address there would make the next enumeration read back a base the chip
// is not decoding.
void Piix4PmReset(Piix4PmDevice* d) {
  FillIdentity(d);
  d->pci.config[kCfgPmBase] = 0;
  d->pci.config[kCfgPmBase + 1] = 0;
  d->pci.config[kCfgPmRegMisc] = 0;
  d->pmregmisc = 0;
  d->base = 0;
  d->pm1_sts = 0;
  d->pm1_en = 0;
  d->pm1_cnt = 0;
  d->last_timer = PmTicks();
  if (d->set_sci) d->set_sci(d->sci_ctx, kSciIrq, 0);
  BusRemoveRegion(d->io, &kPmOps, d);
  // QEMU's reset leaves the SMBus block and its configuration image alone
  // (hw/acpi/piix4.c piix4_pm_reset touches neither), so the register file and
  // the bus population survive; what a reset must restore is the decode, since
  // this model re-publishes its windows from scratch.
  SmbMap(d);
}

void Piix4PmSetIrqSink(Piix4PmDevice* d, void (*set_sci)(void* ctx, int line, int level),
                       void* ctx) {
  d->set_sci = set_sci;
  d->sci_ctx = ctx;
}

void Piix4PmSetShutdownSink(Piix4PmDevice* d, void (*request_shutdown)(void* ctx),
                            void* ctx) {
  d->request_shutdown = request_shutdown;
  d->shutdown_ctx = ctx;
}

void Piix4SmbusAttach(Piix4PmDevice* d, SmbusSlave* slave) {
  if (d->smb.slave_count >= kSmbusSlaveCount) return;
  d->smb.slaves[d->smb.slave_count++] = slave;
}

// The PM timer's overflow is a fixed event: bit 0 of PM1_STS and, when enabled
// and SCI_EN is set, an SCI. The counter is free-running, so the wrap is
// detected by comparing samples of the same 32-bit value the guest reads.
void Piix4PmPoll(Piix4PmDevice* d) {
  const uint32_t t = PmTicks();
  if (t < d->last_timer) {
    d->pm1_sts |= kPm1StsTimer;
    PmUpdateSci(d);
  }
  d->last_timer = t;
}
