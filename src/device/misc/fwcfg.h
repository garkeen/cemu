#ifndef CEMU_DEVICE_MISC_FWCFG_H
#define CEMU_DEVICE_MISC_FWCFG_H

#include <stdint.h>

#include "bus/bus.h"

// QEMU fw_cfg at the classic x86 ports (docs/specs/fw_cfg.txt): the selector
// is written as a 16-bit value to 0x510, the selected entry's bytes are read
// one at a time (little-endian, auto-advancing) from 0x511. Entries this
// machine actually has are served with their QEMU-contract values; unknown
// selectors read as zero bytes (QEMU's behavior for absent entries).
typedef struct FwCfgDevice {
  uint16_t selector;
  uint64_t offset;
} FwCfgDevice;

void FwCfgInit(FwCfgDevice* d);
void FwCfgRegister(Bus* io, FwCfgDevice* d);

extern const DeviceOps kFwCfgOps;

#endif
