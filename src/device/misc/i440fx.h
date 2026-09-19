#ifndef CEMU_DEVICE_MISC_I440FX_H
#define CEMU_DEVICE_MISC_I440FX_H

#include <stdint.h>

#include "device/misc/pci.h"

// Intel 82441FX (i440FX) PCI/ISA host bridge — as much of it as firmware
// needs: the identity 8086:1237 that makes SeaBIOS find it, and the PAM
// (programmable attribute map) registers 0x59-0x5F that decide whether the
// BIOS area (0xC0000-0xFFFFF) is answered by the ROM or by RAM. SeaBIOS's
// shadowing code writes exactly these bytes: 0x30 for the F-segment and 0x33
// for the six 32KiB regions below it while making the BIOS writable, then
// 0x10/0x11 to write-protect it again (seabios src/fw/shadow.c
// __make_bios_writable_intel / make_bios_readonly_intel, Intel 82441FX
// datasheet §4.1).
//
// Bit layout, as those values require: each of PAM1..6 covers two 16KiB halves
// with write enable in bit 1 (low half) / bit 5 (high half) and read enable in
// bit 0 / bit 4; PAM0's single 64KiB region (0xF0000-0xFFFFF) uses the upper
// pair. Reset leaves every region on the ROM side, which is what makes
// SeaBIOS run its shadowing routine from the high flash alias.
typedef struct I440fxDevice {
  PciDevice pci;
} I440fxDevice;

void I440fxInit(I440fxDevice* d, uint8_t bus, uint8_t dev);
// 1 when PAM hands this BIOS-area address to RAM for writing: the ROM window
// then takes the store instead of dropping it. Reads always see the image,
// which holds the same bytes RAM was initialized with.
int I440fxRamWritable(const I440fxDevice* d, uint64_t addr);

#endif
