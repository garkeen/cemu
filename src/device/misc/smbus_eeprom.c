#include "device/misc/smbus_eeprom.h"

#include <string.h>

// Reads hand out data[offset] and step the address register on; the register is
// one byte wide, so it wraps at the array's size by itself (hw/i2c/smbus_eeprom.c
// eeprom_receive_byte).
static uint8_t EepromReceiveByte(void* ctx) {
  SmbusEeprom* e = (SmbusEeprom*)ctx;
  return e->data[e->offset++];
}

// The bytes a write phase collected: the first names the offset, the rest land
// from there in order (eeprom_write_data).
static void EepromWriteData(void* ctx, const uint8_t* buf, int len) {
  SmbusEeprom* e = (SmbusEeprom*)ctx;
  e->offset = buf[0];
  for (int i = 1; i < len; i++) e->data[e->offset++] = buf[i];
}

void SmbusEepromAttach(SmbusEeprom* e, Piix4PmDevice* pm, uint8_t address) {
  memset(e, 0, sizeof(*e));
  e->slave.address = address;
  e->slave.ReceiveByte = EepromReceiveByte;
  e->slave.WriteData = EepromWriteData;
  e->slave.ctx = e;
  Piix4SmbusAttach(pm, &e->slave);
}

void SmbusEepromReset(SmbusEeprom* e) {
  memset(e->data, 0, sizeof(e->data));
  e->offset = 0;
}
