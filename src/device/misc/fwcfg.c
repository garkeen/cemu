#include "device/misc/fwcfg.h"

#include <string.h>

#include "util/log.h"

// Port layout and entry semantics: QEMU docs/specs/fw_cfg.txt (the io
// interface: selector at BIOS_CFG_IOPORT 0x510, 8-bit data at 0x511).
enum { kFwCfgSelectorPort = 0x510, kFwCfgDataPort = 0x511 };

// Selectors (QEMU docs/specs/fw_cfg.txt classic table: 0x05 is NB_CPUS).
enum {
  kFwCfgSignature = 0x00,
  kFwCfgId = 0x01,
  kFwCfgRamSize = 0x03,
  kFwCfgNbCpus = 0x05,
};

// This machine's honest answers: 32MB of RAM, one hart. The signature is
// "QEMU" because the port contract this device implements (and the guests'
// fw_cfg drivers) is QEMU's.
static const uint8_t kSignature[4] = {'Q', 'E', 'M', 'U'};
static const uint64_t kRamSize = 32ULL << 20;
static const uint16_t kNbCpus = 1;

// One byte of the file directory: a big-endian 32-bit entry count followed by
// 64-byte entries of {be32 size, be16 select, be16 reserved, char name[56]}
// (QEMU docs/specs/fw_cfg.txt; seabios src/fw/paravirt.c parses exactly that
// layout).
enum {
  kFwCfgFileDirCountLen = 4,
  kFwCfgFileEntryLen = 64,
};

static int FwCfgDirByte(FwCfgDevice* d, uint64_t i) {
  // The count and every entry field are big-endian (QEMU docs/specs/fw_cfg.txt;
  // seabios be32_to_cpu/be16_to_cpu on the way in).
  if (i < kFwCfgFileDirCountLen)
    return (int)(((uint32_t)d->file_count >> (8 * (3 - i))) & 0xff);
  uint64_t rest = i - kFwCfgFileDirCountLen;
  int file = (int)(rest / kFwCfgFileEntryLen);
  uint64_t off = rest % kFwCfgFileEntryLen;
  if (file >= d->file_count) return -1;
  const FwCfgFile* f = &d->files[file];
  if (off < 4) return (int)((f->size >> (8 * (3 - off))) & 0xff);
  if (off < 6) {
    uint16_t key = (uint16_t)(kFwCfgFileFirstSelect + file);
    return (int)((key >> (8 * (5 - off))) & 0xff);
  }
  if (off < 8) return 0;  // reserved
  if (off < 8 + kFwCfgFileNameLen) return (int)(uint8_t)f->name[off - 8];
  return 0;
}

// Returns the entry's byte at offset i, or -1 when the entry is absent or
// the offset is past its length.
static int FwCfgByte(FwCfgDevice* d, uint64_t i) {
  switch (d->selector) {
    case kFwCfgSignature:
      return i < 4 ? kSignature[i] : -1;
    case kFwCfgId:
      return i < 4 ? 0 : -1;
    case kFwCfgRamSize:
      return i < 8 ? (int)((kRamSize >> (8 * i)) & 0xff) : -1;
    case kFwCfgNbCpus:
      return i < 2 ? (int)((kNbCpus >> (8 * i)) & 0xff) : -1;
    case kFwCfgFileDirSelect:
      return FwCfgDirByte(d, i);
    default:
      break;
  }
  // A published romfile reads back through its own select key; every other
  // selector is an entry this machine does not have (QEMU answers those with
  // zero bytes).
  int file = (int)d->selector - kFwCfgFileFirstSelect;
  if (file >= 0 && file < d->file_count) {
    const FwCfgFile* f = &d->files[file];
    return i < f->size ? (int)f->data[i] : -1;
  }
  return -1;
}

static uint64_t FwCfgRead(void* dev, uint64_t addr, int size) {
  (void)size;
  FwCfgDevice* d = (FwCfgDevice*)dev;
  if (addr != kFwCfgDataPort) return 0;  // the selector port is write-only
  int b = FwCfgByte(d, d->offset);
  d->offset++;
  return b < 0 ? 0 : (uint64_t)b;
}

static void FwCfgWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  FwCfgDevice* d = (FwCfgDevice*)dev;
  if (addr == kFwCfgSelectorPort) {
    // The selector register is 16 bits; 32-bit outs (what a 32-bit code
    // segment's `out %eax` compiles to) land here too and take the low half,
    // matching QEMU's portio behavior.
    d->selector = (uint16_t)val;
    d->offset = 0;
  }
  // The data port is read-only; QEMU ignores writes to it on the io interface.
}

const DeviceOps kFwCfgOps = {"fwcfg", FwCfgRead, FwCfgWrite};

void FwCfgInit(FwCfgDevice* d) {
  d->selector = 0;
  d->offset = 0;
  d->file_count = 0;
}

int FwCfgAddFile(FwCfgDevice* d, const char* name, const uint8_t* data, uint32_t size) {
  size_t len = strlen(name);
  if (len == 0 || len >= kFwCfgFileNameLen) {
    LogError("fw_cfg file name '%s' does not fit a directory entry", name);
    return -1;
  }
  if (d->file_count >= kFwCfgMaxFiles) {
    LogError("fw_cfg file table is full (%d entries)", kFwCfgMaxFiles);
    return -1;
  }
  FwCfgFile* f = &d->files[d->file_count++];
  memset(f, 0, sizeof(*f));
  memcpy(f->name, name, len);
  f->data = data;
  f->size = size;
  return 0;
}

void FwCfgRegister(Bus* io, FwCfgDevice* d) {
  BusAddRegion(io, kFwCfgSelectorPort, 4, &kFwCfgOps, d);
}
