#ifndef CEMU_DEVICE_STORAGE_OBERON_SD_H
#define CEMU_DEVICE_STORAGE_OBERON_SD_H

#include <stdint.h>

#include "host/host.h"

// The Oberon machine's SD card: an SPI-mode card model with exactly the
// command coverage the reference machine's disk device has
// (oberon-riscv-emu src/disk.c, ported behavior for behavior). Two commands
// are real, both with 512-byte blocks:
//
//   CMD17 (0x51) read single block: reply 0x00, then 0xFE start-of-data,
//               then the 128 words of the block
//   CMD24 (0x58) write single block: reply 0x00, collect the 0xFE token,
//               then 128 words plus two discarded checksum bytes, then
//               reply the 0x05 data-accepted token
//
// Every other command answers with a single 0x00 byte and nothing else. That
// is a calibrated behavior, not an omission: the Oberon kernel's SDShift
// probes CMD58 (OCR) and reads the CCS capacity bit out of the follow-up
// bytes, which come back as the idle 0xFF fill — so the card presents as
// block-addressed (SDHC), the addressing Kernel.GetSector/PutSector use.
// The model also looks for the Oberon FileDir disk marker (0x9B1EA38D, word
// 0 of block 1) and re-bases block numbers by 0x80002 for such images, the
// reference device's support for filesystem-only disk files. Both make this
// file Oberon-named: the behavior is tuned to this machine's disk driver,
// and another guest's SD init sequence (CMD0/CMD8/ACMD41) would never
// complete against it.
//
// Exchange protocol: the machine's SPI is write-then-read per byte — the
// guest stores a byte, then loads the byte clocked out during the same
// exchange. Between transactions the card fills with 0xFF.
typedef struct OberonSdDevice {
  enum OberonSdState {
    kSdCommand,    // collecting command bytes
    kSdRead,       // serving a CMD17 response stream
    kSdWaitToken,  // CMD24 issued, waiting for the 0xFE write token
    kSdStore,      // collecting the block to write
  } state;
  HostFile* file;
  uint64_t pos;     // byte offset the last CMD17/CMD24 sought to
  uint32_t offset;  // block-number rebasing for FileDir-marker images
  uint32_t rx_buf[128];
  int rx_idx;
  uint32_t tx_buf[128 + 2];
  int tx_cnt;
  int tx_idx;
} OberonSdDevice;

// path NULL: no card inserted (reads serve zeroed blocks, writes go nowhere —
// the reference's diskless boot). Returns 0, or -1 with the error logged when
// the image cannot be opened.
int OberonSdInit(OberonSdDevice* d, const char* path);
void OberonSdDestroy(OberonSdDevice* d);
// The SPI data register's two halves.
uint32_t OberonSdRead(OberonSdDevice* d);
void OberonSdWrite(OberonSdDevice* d, uint32_t value);

#endif  // CEMU_DEVICE_STORAGE_OBERON_SD_H
