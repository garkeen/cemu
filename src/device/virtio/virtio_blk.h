#ifndef CEMU_DEVICE_VIRTIO_VIRTIO_BLK_H
#define CEMU_DEVICE_VIRTIO_VIRTIO_BLK_H

#include <stdint.h>

#include "bus/bus.h"
#include "device/virtio/virtio.h"

// VirtIO block device (spec 5.2) on a host raw image file. One request =
// one descriptor chain: a 16-byte request header (device-readable), the data
// buffer (direction by request type), and one status byte
// (device-writable). Requests run synchronously against the host file at
// QueueNotify time.

typedef struct VirtioBlk {
  VirtioDevice vdev;
  void* file;  // host file handle (util layer), NULL = no backend
  uint64_t sectors;
} VirtioBlk;

// Attaches the device to the bus window at [base, base+size); file_path is
// the host image (opened read/write). Returns NULL when the image cannot be
// opened -- the board then leaves the slot empty, exactly like QEMU's
// virtio-blk-device without a drive.
VirtioBlk* VirtioBlkCreate(Bus* bus, uint64_t base, uint64_t size, const char* file_path,
                           int irq_src, void (*set_irq)(void*, int, int), void* irq_ctx);
void VirtioBlkDestroy(VirtioBlk* blk);

#endif
