#ifndef CEMU_DEVICE_MISC_FWCFG_H
#define CEMU_DEVICE_MISC_FWCFG_H

#include <stdint.h>

#include "bus/bus.h"

// QEMU fw_cfg at the classic x86 ports (docs/specs/fw_cfg.txt): the selector
// is written as a 16-bit value to 0x510, the selected entry's bytes are read
// one at a time (little-endian, auto-advancing) from 0x511. Entries this
// machine actually has are served with their QEMU-contract values; unknown
// selectors read as zero bytes (QEMU's behavior for absent entries).
//
// The file interface is the other half of that contract: entry 0x19 is a
// directory (big-endian count, then one 64-byte entry per romfile) and a
// file's bytes are read back by selecting its own key. Firmware uses it for
// everything that is not one of the fixed legacy entries — on this machine,
// for the display card's option ROM (SeaBIOS src/optionroms.c
// run_file_roms("vgaroms/"), which is how a VGA BIOS with no PCI device of
// its own reaches the option ROM area).
enum {
  // The directory entry's key (QEMU_CFG_FILE_DIR in seabios paravirt.c).
  kFwCfgFileDirSelect = 0x19,
  // QEMU's FW_CFG_FILE_FIRST: the key of directory entry 0. The keys are
  // consecutive, so entry i is read through kFwCfgFileFirstSelect + i.
  kFwCfgFileFirstSelect = 0x20,
  // The directory entry's name field (QEMU's struct QEMUFileEntry); names
  // shorter than this are NUL-padded, and a longer name cannot be published.
  kFwCfgFileNameLen = 56,
  kFwCfgMaxFiles = 4,
};

typedef struct FwCfgFile {
  char name[kFwCfgFileNameLen];
  const uint8_t* data;
  uint32_t size;
} FwCfgFile;

typedef struct FwCfgDevice {
  uint16_t selector;
  uint64_t offset;
  FwCfgFile files[kFwCfgMaxFiles];
  int file_count;
} FwCfgDevice;

void FwCfgInit(FwCfgDevice* d);
void FwCfgRegister(Bus* io, FwCfgDevice* d);
// Publishes one romfile: the directory lists it by name and its bytes are read
// back through its own select key. The data must outlive the machine. Returns
// 0, or -1 when the table is full or the name does not fit (a programming
// error: reported rather than truncated silently).
int FwCfgAddFile(FwCfgDevice* d, const char* name, const uint8_t* data, uint32_t size);

extern const DeviceOps kFwCfgOps;

#endif
