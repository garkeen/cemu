#ifndef CEMU_DEVICE_MISC_SMBUS_EEPROM_H
#define CEMU_DEVICE_MISC_SMBUS_EEPROM_H

#include <stdint.h>

#include "device/misc/piix4pm.h"

// The serial EEPROM an SPD (serial presence detect) bank is made of, as the
// PIIX4's SMBus finds it: QEMU's pc machine attaches eight of them at 0x50-0x57
// with no image loaded (hw/i386/pc_piix.c smbus_eeprom_init(pcms->smbus, 8, NULL,
// 0), whose TODO is the missing SPD data), and its device model
// (hw/i2c/smbus_eeprom.c) is two rules:
//
//   write  the first byte of the write phase is the offset, the following ones
//          are stored from there, and the offset follows them;
//   read   every byte handed out is data[offset] and steps the offset on.
//
// The offset is one byte wide, so both wrap at the array's size.
enum {
  kSmbusSpdCount = 8,       // the bank QEMU's pc machine attaches
  kSmbusSpdAddress = 0x50,  // where the first SPD answers
  kSmbusEepromSize = 256,   // SMBUS_EEPROM_SIZE
};

typedef struct SmbusEeprom {
  SmbusSlave slave;  // the SMBus-side identity and protocol hooks
  uint8_t data[kSmbusEepromSize];
  uint8_t offset;    // the address register, stepped by every read
} SmbusEeprom;

// Puts the EEPROM on the bus. The contents start as all zeros, which is what
// QEMU's smbus_eeprom_init leaves behind when it is handed no SPD image.
void SmbusEepromAttach(SmbusEeprom* e, Piix4PmDevice* pm, uint8_t address);

// Restores the initial contents, the way QEMU's device reset does: an EEPROM
// with a bus write in it goes back to what a power cycle would find.
void SmbusEepromReset(SmbusEeprom* e);

#endif
