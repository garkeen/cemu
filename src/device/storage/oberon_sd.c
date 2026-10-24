#include "device/storage/oberon_sd.h"

#include <string.h>

#include "util/log.h"

// Block geometry: the card's blocks are 512 bytes, 128 words, little-endian
// in both the image file and the SPI stream (disk.c read_sector/write_sector).
enum {
  kSdBlockWords = 128,
  kSdCmdBytes = 6,
  kSdReadToken = 254,   // 0xFE: start-of-data, both directions
  kSdDataAccepted = 5,  // 0x05: the write path's data-response token
};

static void SeekSector(OberonSdDevice* d, uint32_t block) {
  d->pos = (uint64_t)block * (kSdBlockWords * 4);
}

static void ReadSector(OberonSdDevice* d, uint32_t out[kSdBlockWords]) {
  uint8_t bytes[kSdBlockWords * 4];
  memset(bytes, 0, sizeof(bytes));
  if (d->file) HostFileReadAt(d->file, (int64_t)d->pos, bytes, sizeof(bytes));
  for (int i = 0; i < kSdBlockWords; i++) {
    out[i] = (uint32_t)bytes[i * 4] | (uint32_t)bytes[i * 4 + 1] << 8 |
             (uint32_t)bytes[i * 4 + 2] << 16 | (uint32_t)bytes[i * 4 + 3] << 24;
  }
}

static void WriteSector(OberonSdDevice* d, const uint32_t in[kSdBlockWords]) {
  uint8_t bytes[kSdBlockWords * 4];
  for (int i = 0; i < kSdBlockWords; i++) {
    bytes[i * 4] = (uint8_t)in[i];
    bytes[i * 4 + 1] = (uint8_t)(in[i] >> 8);
    bytes[i * 4 + 2] = (uint8_t)(in[i] >> 16);
    bytes[i * 4 + 3] = (uint8_t)(in[i] >> 24);
  }
  if (d->file) HostFileWriteAt(d->file, (int64_t)d->pos, bytes, sizeof(bytes));
}

static void RunCommand(OberonSdDevice* d) {
  uint32_t cmd = d->rx_buf[0];
  uint32_t arg = d->rx_buf[1] << 24 | d->rx_buf[2] << 16 | d->rx_buf[3] << 8 | d->rx_buf[4];
  switch (cmd) {
    case 81:  // CMD17: 0x40|17, read one block
      d->state = kSdRead;
      d->tx_buf[0] = 0;
      d->tx_buf[1] = kSdReadToken;
      SeekSector(d, arg - d->offset);
      ReadSector(d, &d->tx_buf[2]);
      d->tx_cnt = 2 + kSdBlockWords;
      break;
    case 88:  // CMD24: 0x40|24, write one block
      d->state = kSdWaitToken;
      SeekSector(d, arg - d->offset);
      d->tx_buf[0] = 0;
      d->tx_cnt = 1;
      break;
    default:
      // One 0x00 byte and silence — the answer SDShift's OCR probe reads its
      // capacity bit out of (oberon_sd.h).
      d->tx_buf[0] = 0;
      d->tx_cnt = 1;
      break;
  }
  d->tx_idx = -1;
}

int OberonSdInit(OberonSdDevice* d, const char* path) {
  memset(d, 0, sizeof(*d));
  d->state = kSdCommand;
  if (path) {
    d->file = HostFileOpenReadWrite(path);
    if (!d->file) {
      LogError("oberon: cannot open disk image '%s'", path);
      return -1;
    }
    // A filesystem-only image carries the Oberon FileDir marker in word 0 of
    // block 1; its block numbers are re-based past the empty boot area
    // (disk.c disk_new).
    ReadSector(d, &d->tx_buf[0]);
    d->offset = d->tx_buf[0] == 0x9B1EA38D ? 0x80002 : 0;
    d->tx_cnt = 0;
    d->tx_idx = 0;
    d->state = kSdCommand;
  }
  return 0;
}

void OberonSdDestroy(OberonSdDevice* d) {
  if (d->file) HostFileClose(d->file);
  d->file = NULL;
}

void OberonSdWrite(OberonSdDevice* d, uint32_t value) {
  d->tx_idx++;
  switch (d->state) {
    case kSdCommand:
      // A 0xFF before the first command byte is bus idle (the guest's dummy
      // clocks), not data; once collection starts every byte counts — the
      // argument may legitimately contain 0xFF (disk.c disk_write).
      if ((uint8_t)value != 0xFF || d->rx_idx != 0) {
        d->rx_buf[d->rx_idx] = value;
        d->rx_idx++;
        if (d->rx_idx == kSdCmdBytes) {
          RunCommand(d);
          d->rx_idx = 0;
        }
      }
      break;
    case kSdRead:
      // Each dummy byte clocks one response word out; past the response the
      // transaction closes and the card idles (reads then return 0xFF).
      if (d->tx_idx == d->tx_cnt) {
        d->state = kSdCommand;
        d->tx_cnt = 0;
        d->tx_idx = 0;
      }
      break;
    case kSdWaitToken:
      if (value == kSdReadToken) d->state = kSdStore;
      break;
    case kSdStore:
      if (d->rx_idx < kSdBlockWords) d->rx_buf[d->rx_idx] = value;
      d->rx_idx++;
      if (d->rx_idx == kSdBlockWords) {
        WriteSector(d, d->rx_buf);
      } else if (d->rx_idx == kSdBlockWords + 2) {
        // Two checksum bytes later: report data accepted, back to commands.
        d->tx_buf[0] = kSdDataAccepted;
        d->tx_cnt = 1;
        d->tx_idx = -1;
        d->rx_idx = 0;
        d->state = kSdCommand;
      }
      break;
  }
}

uint32_t OberonSdRead(OberonSdDevice* d) {
  if (d->tx_idx >= 0 && d->tx_idx < d->tx_cnt) return d->tx_buf[d->tx_idx];
  return 255;
}
