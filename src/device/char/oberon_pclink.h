#ifndef CEMU_DEVICE_CHAR_OBERON_PCLINK_H
#define CEMU_DEVICE_CHAR_OBERON_PCLINK_H

#include <stdint.h>

#include "host/host.h"

// The Oberon machine's serial port backend: the PCLink1 file channel, ported
// behavior for behavior from the reference emulator's pclink.c (itself from
// Peter De Wachter's RISC emulator). The Oberon side is PCLink1.Mod; the host
// side is a pair of job files in the working directory the emulator is run
// from, exactly as the reference documents it:
//
//   echo SomeFile.Mod > PCLink.REC   deliver that file to the guest
//   echo SomeFile.Mod > PCLink.SND   receive that file from the guest
//
// The wire protocol over the serial data register: the guest polls status
// (bit 0 = a job is pending, bit 1 = transmit ready — always set); a job
// starts with the REC (0x21) or SND (0x22) tag, then the NUL-terminated
// filename, then the payload. A receive delivers the file in 256-byte frames,
// each led by its length byte (0 = end); a send collects the same frames, and
// the host swaps job files for ACK (0x10) bytes until the file completes.
// Anything but ACK at the start aborts the job and cleans up after it.
typedef struct OberonPclinkDevice {
  uint8_t mode;  // 0 idle, 0x21 receiving, 0x22 sending
  HostFile* file;
  int64_t file_pos;     // sequential cursor for the open payload
  int txcount, rxcount;
  int fnlen, flen;      // filename length (with NUL); payload bytes left (-1 = unknown)
  char filename[32];
  uint8_t buf[257];
} OberonPclinkDevice;

void OberonPclinkInit(OberonPclinkDevice* d);
void OberonPclinkDestroy(OberonPclinkDevice* d);
// The serial data/status registers' three halves (risc-io.h RISC_Serial).
uint32_t OberonPclinkReadData(OberonPclinkDevice* d);
uint32_t OberonPclinkReadStatus(OberonPclinkDevice* d);
void OberonPclinkWriteData(OberonPclinkDevice* d, uint32_t value);

#endif  // CEMU_DEVICE_CHAR_OBERON_PCLINK_H
