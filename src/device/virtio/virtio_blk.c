// VirtIO block device (spec 5.2, legacy feature word). Request layout and
// the status byte follow the spec's "Basic Requests"; the backend is the
// host file layer (host/host.h), accessed synchronously -- one notify drains
// the whole avail ring. Reference: dearchap-tinyemu virtio.c
// virtio_block_recv_request, calibrated against QEMU hw/virtio/virtio-blk.

#include <stdlib.h>
#include <string.h>

#include "device/virtio/virtio_blk.h"
#include "host/host.h"
#include "util/log.h"

enum {
  kVirtioBlkTIn = 0,
  kVirtioBlkTOut = 1,
  kVirtioBlkTFlush = 4,
};
enum {
  kVirtioBlkOk = 0,
  kVirtioBlkIoerr = 1,
};

enum { kSectorSize = 512, kBlkHeaderSize = 16 };

typedef struct BlkHeader {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
} BlkHeader;

static int BlkRecv(VirtioDevice* v, int q, int desc, int rd, int wr) {
  VirtioBlk* blk = (VirtioBlk*)v;
  BlkHeader h;
  if (rd < (int)sizeof h) return 0;  // malformed: retire the chain
  if (VirtioCopyFromChain(v, q, desc, 0, &h, sizeof h)) return 0;

  uint8_t status = kVirtioBlkOk;
  if (h.type == kVirtioBlkTIn) {
    // writable run = data buffer + status byte at its end
    int data_len = wr - 1;
    uint8_t* buf = malloc(data_len);
    if (!blk->file ||
        HostFileReadAt((HostFile*)blk->file, (int64_t)(h.sector * kSectorSize), buf,
                       data_len) != (size_t)data_len) {
      status = kVirtioBlkIoerr;
      data_len = 0;
    }
    if (data_len > 0) VirtioCopyToChain(v, q, desc, 0, buf, data_len);
    free(buf);
  } else if (h.type == kVirtioBlkTOut) {
    int data_len = rd - kBlkHeaderSize;
    uint8_t* buf = malloc(data_len);
    if (VirtioCopyFromChain(v, q, desc, kBlkHeaderSize, buf, data_len) != 0 || !blk->file ||
        HostFileWriteAt((HostFile*)blk->file, (int64_t)(h.sector * kSectorSize), buf,
                        data_len) != (size_t)data_len) {
      status = kVirtioBlkIoerr;
    }
    free(buf);
  } else if (h.type == kVirtioBlkTFlush) {
    // host writes go straight through the OS (write-through cache)
  } else {
    return 0;
  }
  // the status byte is the last byte of the writable run
  VirtioCopyToChain(v, q, desc, wr - 1, &status, 1);
  VirtioConsumeChain(v, q, desc, wr);
  return 0;
}

VirtioBlk* VirtioBlkCreate(Bus* bus, uint64_t base, uint64_t size, const char* file_path,
                           int irq_src, void (*set_irq)(void*, int, int), void* irq_ctx) {
  HostFile* f = HostFileOpenReadWrite(file_path);
  if (!f) return NULL;
  VirtioBlk* blk = calloc(1, sizeof *blk);
  VirtioInit(&blk->vdev, bus, 2 /*virtio-blk id, spec 5.2*/, kVirtioVendorId,
             0 /*legacy: no optional features*/, 1 /*legacy transport*/,
             8 /*capacity field*/, BlkRecv);
  blk->file = f;
  blk->sectors = HostFileSize(f) / kSectorSize;
  uint64_t cap = blk->sectors;
  memcpy(blk->vdev.config, &cap, 8);
  blk->vdev.irq_src = irq_src;
  blk->vdev.set_irq = set_irq;
  blk->vdev.irq_ctx = irq_ctx;
  BusAddRegion(bus, base, size, &kVirtioMmioOps, &blk->vdev);
  return blk;
}

void VirtioBlkDestroy(VirtioBlk* blk) {
  if (!blk) return;
  if (blk->file) HostFileClose((HostFile*)blk->file);
  free(blk);
}
