#ifndef CEMU_DEVICE_MISC_CMOS_H
#define CEMU_DEVICE_MISC_CMOS_H

#include <stdint.h>

#include "bus/bus.h"

// MC146818 real-time clock and the 128-byte CMOS RAM as wired on the PC:
// index port 0x70 (bit 7 disables the NMI), data port 0x71. Firmware and the
// OS use it for the clock, and — before there is any other way to ask — for
// how much memory the machine has: registers 0x30/0x31 hold the memory above
// 1MiB in KiB and 0x34/0x35 the memory above 16MiB in 64KiB units
// (seabios src/hw/rtc.h CMOS_MEM_EXTMEM*, src/fw/paravirt.c qemu_preinit),
// which is the first thing POST reads to size RAM.
//
// The clock keeps host wall-clock time (UTC): reads answer the live time in
// the format the status register asks for (BCD/binary, 12/24 hour), writes set
// the clock by biasing the host clock, and the update-in-progress bit never
// reads set — an emulated RTC updates instantaneously.
typedef struct CmosDevice {
  struct CmosState* st;  // private state: index register + CMOS RAM + clock bias
} CmosDevice;

void CmosInit(CmosDevice* d);
// A machine reset (D18): the battery-backed contents survive it, the access
// state does not.
void CmosReset(CmosDevice* d);
void CmosRegister(Bus* io, CmosDevice* d);
// Tells the RTC how much RAM the machine carries; the extended-memory
// registers answer from it.
void CmosSetMemory(CmosDevice* d, uint64_t ram_size);

#endif
