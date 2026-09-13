#include "device/misc/fwcfg.h"

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
    default:
      return -1;
  }
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
}

void FwCfgRegister(Bus* io, FwCfgDevice* d) {
  BusAddRegion(io, kFwCfgSelectorPort, 4, &kFwCfgOps, d);
}
