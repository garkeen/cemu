#ifndef CEMU_DEVICE_VIRTIO_VIRTIO_H
#define CEMU_DEVICE_VIRTIO_VIRTIO_H

#include <stdint.h>

#include "bus/bus.h"

// VirtIO over MMIO, legacy revision 1 (the register layout of the "Virtio
// over MMIO" appendix before VirtIO 1.0's revision 2; offset constants below
// match include/uapi/linux/virtio_mmio.h and dearchap-tinyemu virtio.c, and
// the handshake semantics are calibrated against QEMU 10.2's virtio-mmio in
// force-legacy mode -- the transport this machine's guests actually speak,
// captured with QEMU's virtio_mmio trace events).
//
// One VirtioDevice is one slot on the virtio-mmio bus: a register file, a set
// of split virtqueues living in guest RAM, a config-space page the backend
// fills, and one interrupt line the board wires to the PLIC. The backend
// (blk/console/input/...) supplies device_id, feature bits, the config page
// and a recv callback that handles one available descriptor chain; the
// transport owns everything the VirtIO spec calls the transport's business:
// register decoding, queue addressing, avail/used rings and the interrupt.
//
// Requests are handled synchronously at QueueNotify time (the emulator is
// single-threaded; a guest polling the used ring and an interrupt-driven one
// both make progress).

enum {
  kVirtioMmioMagic = 0x000,
  kVirtioMmioVersion = 0x004,
  kVirtioMmioDeviceId = 0x008,
  kVirtioMmioVendorId = 0x00c,
  kVirtioMmioDeviceFeatures = 0x010,
  kVirtioMmioDeviceFeaturesSel = 0x014,
  kVirtioMmioDriverFeatures = 0x020,
  kVirtioMmioDriverFeaturesSel = 0x024,
  kVirtioMmioGuestPageSize = 0x028,  // legacy only
  kVirtioMmioQueueSel = 0x030,
  kVirtioMmioQueueNumMax = 0x034,
  kVirtioMmioQueueNum = 0x038,
  kVirtioMmioQueueAlign = 0x03c,  // legacy only
  kVirtioMmioQueuePfn = 0x040,    // legacy only
  kVirtioMmioQueueNotify = 0x050,
  kVirtioMmioInterruptStatus = 0x060,
  kVirtioMmioInterruptAck = 0x064,
  kVirtioMmioStatus = 0x070,
  kVirtioMmioConfig = 0x100,
};

// Legacy device status bits (VirtIO 1.1 spec 2.1).
enum {
  kVirtioStatusAcknowledge = 1u << 0,
  kVirtioStatusDriver = 1u << 1,
  kVirtioStatusDriverOk = 1u << 2,
  kVirtioStatusFailed = 1u << 7,
};

// Descriptor flags (spec 2.6.5).
enum {
  kVringDescFNext = 1u << 0,
  kVringDescFWrite = 1u << 1,
};

// Interrupt status bits: bit 0 = a used ring was updated (spec 4.1.4.3).
enum { kVirtioIntUsedRing = 1u << 0 };

// The 4.15 virtio_console driver builds two queues per port for as many ports
// as the config page advertises (QEMU's virtio-serial defaults to 31), so a
// console slot can legitimately come up with 64 queues.
enum {
  kVirtioMaxQueue = 64,
  kVirtioMaxQueueNum = 1024,
  kVirtioConfigSize = 256,
  // Vendor ID every cemu virtio-mmio transport reports ("CUM " little-endian).
  kVirtioVendorId = 0x4345554d,
};

typedef struct VirtioQueue {
  uint32_t num;            // queue size in descriptors (power of two)
  uint32_t pfn;            // legacy: written by the driver, read back
  uint64_t desc_addr;      // guest physical, derived from pfn << page_shift
  uint64_t avail_addr;
  uint64_t used_addr;
  uint16_t last_avail_idx;
  int manual;              // the transport does not drain this queue on
                           // notify; the backend pulls it (console receive)
} VirtioQueue;

typedef struct VirtioDevice VirtioDevice;

// Backend hook: one available descriptor chain (chain head `desc`, with
// rd/wr bytes of device-readable/device-writable data). Return 0 when the
// chain is consumed (the transport advances last_avail_idx), -1 to leave it
// for later (busy, or a manually drained queue).
typedef int (*VirtioRecvFn)(VirtioDevice* v, int queue, int desc, int rd, int wr);
typedef void (*VirtioResetFn)(VirtioDevice* v);
typedef void (*VirtioConfigWriteFn)(VirtioDevice* v);

struct VirtioDevice {
  Bus* bus;
  uint32_t device_id, vendor_id;
  uint32_t device_features;  // legacy: bits 0..31
  uint32_t guest_features;
  uint32_t status;
  uint32_t features_sel, queue_sel;
  uint32_t int_status;
  uint32_t page_shift;  // from GuestPageSize (legacy)
  uint32_t align;       // from QueueAlign (legacy), bytes
  VirtioQueue queue[kVirtioMaxQueue];
  uint8_t config[kVirtioConfigSize];
  int config_size;  // valid prefix of the config page the backend fills
  VirtioRecvFn recv;
  VirtioResetFn reset_hook;        // optional
  VirtioConfigWriteFn config_hook;  // optional
  int irq_src;  // the line number the board gave this slot (PLIC source)
  void (*set_irq)(void* ctx, int src, int level);
  void* irq_ctx;
};

extern const DeviceOps kVirtioMmioOps;
// A DTB-advertised slot with no backend attached: QEMU's virtio-mmio keeps
// magic/version/vendor readable and returns 0 everywhere else (DeviceID 0
// claims no driver), instead of leaving the address unmapped.
extern const DeviceOps kVirtioMmioEmptyOps;

void VirtioInit(VirtioDevice* v, Bus* bus, uint32_t device_id, uint32_t vendor_id,
                uint32_t features, int config_size, VirtioRecvFn recv);

// Queue helpers shared by the backends. Copy between a descriptor chain and
// a linear buffer: `to_guest` picks the first device-writable descriptor and
// fills from there, otherwise the device-readable descriptors are gathered.
int VirtioCopyFromChain(VirtioDevice* v, int queue, int desc, int offset, void* buf,
                        int len);
int VirtioCopyToChain(VirtioDevice* v, int queue, int desc, int offset, const void* buf,
                      int len);
// Publish a finished chain on the used ring and raise the interrupt line.
void VirtioConsumeChain(VirtioDevice* v, int queue, int desc, int written);
// Drain every available chain of a (non-manual) queue through the recv hook.
void VirtioNotifyQueue(VirtioDevice* v, int queue);
// A manual queue's pending chain head, or -1 when empty (console receive).
int VirtioManualChain(VirtioDevice* v, int queue);
// The guest's avail->idx as the transport sees it (diagnostics).
uint16_t VirtioQueueAvail(VirtioDevice* v, int queue);
// Host bytes -> guest: fill a manual chain's writable descriptors.
int VirtioManualFill(VirtioDevice* v, int queue, const void* buf, int len);

#endif
