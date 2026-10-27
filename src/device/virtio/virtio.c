// VirtIO-MMIO transport, legacy revision 1. The register file, split
// virtqueue plumbing and interrupt semantics follow the VirtIO over MMIO
// appendix (pre-1.0 wording, as captured in include/uapi/linux/virtio_mmio.h)
// and are behaviour-calibrated against QEMU 10.2 virtio-mmio force-legacy
// (handshake captured via QEMU's virtio_mmio_* trace events): 32-bit register
// accesses only, queue addressing through QueuePFN/GuestPageSize/QueueAlign,
// used-ring write raises the line until InterruptACK.
//
// The queue's layout under legacy addressing is the one the driver's
// vring_init() computes (include/uapi/linux/virtio_ring.h): descriptors at
// pfn << page_shift, avail ring right after them, used ring aligned past the
// avail ring including its used_event slot.

#include <string.h>

#include "debug/debug.h"
#include "device/virtio/virtio.h"
#include "util/log.h"

enum { kVirtioMagicValue = 0x74726976 };  // "virt" LE, spec 4.2.2

typedef struct VringDesc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} VringDesc;

// ---- guest RAM access ----

static uint8_t* RamPtr(Bus* bus, uint64_t addr, uint64_t len) {
  uint8_t* host;
  if (BusRamRange(bus, addr, len, &host) == 0) return host;
  return NULL;
}

static int ReadPhys(Bus* bus, uint64_t addr, void* buf, uint64_t len) {
  uint8_t* dst = buf;
  while (len > 0) {
    uint64_t page = 0x1000 - (addr & 0xfff);
    uint64_t l = len < page ? len : page;
    uint8_t* host = RamPtr(bus, addr, l);
    if (!host) return -1;
    memcpy(dst, host, l);
    addr += l;
    dst += l;
    len -= l;
  }
  return 0;
}

static int WritePhys(Bus* bus, uint64_t addr, const void* buf, uint64_t len) {
  const uint8_t* src = buf;
  while (len > 0) {
    uint64_t page = 0x1000 - (addr & 0xfff);
    uint64_t l = len < page ? len : page;
    uint8_t* host = RamPtr(bus, addr, l);
    if (!host) return -1;
    memcpy(host, src, l);
    addr += l;
    src += l;
    len -= l;
  }
  return 0;
}

static uint16_t ReadPhys16(Bus* bus, uint64_t addr) {
  uint16_t v;
  return ReadPhys(bus, addr, &v, 2) == 0 ? v : 0;
}

static void WritePhys16(Bus* bus, uint64_t addr, uint16_t v) {
  WritePhys(bus, addr, &v, 2);
}

static void WritePhys32(Bus* bus, uint64_t addr, uint32_t v) {
  WritePhys(bus, addr, &v, 4);
}

static int GetDesc(VirtioDevice* v, int q, int idx, VringDesc* d) {
  VirtioQueue* qs = &v->queue[q];
  return ReadPhys(v->bus, qs->desc_addr + (uint64_t)idx * sizeof(VringDesc), d,
                  sizeof *d);
}

// Read/write sizes of a chain: the device-readable descriptors first, then
// the writable ones (the VirtIO request layout, spec 2.6.5).
static int ChainSizes(VirtioDevice* v, int q, int idx, int* rd, int* wr) {
  VringDesc d;
  int nrd = 0, nwr = 0;
  if (GetDesc(v, q, idx, &d)) return -1;
  for (;;) {
    if (d.flags & kVringDescFWrite) break;
    nrd += d.len;
    if (!(d.flags & kVringDescFNext)) goto done;
    idx = d.next;
    if (GetDesc(v, q, idx, &d)) return -1;
  }
  for (;;) {
    if (!(d.flags & kVringDescFWrite)) return -1;
    nwr += d.len;
    if (!(d.flags & kVringDescFNext)) break;
    idx = d.next;
    if (GetDesc(v, q, idx, &d)) return -1;
  }
done:
  *rd = nrd;
  *wr = nwr;
  return 0;
}

// Walk to the first descriptor with the wanted direction, then copy `len`
// bytes across the chain from `offset` within that run.
static int CopyChain(VirtioDevice* v, int q, int idx, int offset, void* buf, int len,
                     int to_guest) {
  VringDesc d;
  uint8_t* p = buf;
  int want_write = to_guest ? kVringDescFWrite : 0;
  if (len == 0) return 0;
  if (GetDesc(v, q, idx, &d)) return -1;
  if (to_guest) {
    for (;;) {
      if (d.flags & kVringDescFWrite) break;
      if (!(d.flags & kVringDescFNext)) return -1;
      idx = d.next;
      if (GetDesc(v, q, idx, &d)) return -1;
    }
  }
  for (;;) {
    if ((d.flags & kVringDescFWrite) != (uint16_t)want_write) return -1;
    if (offset < (int)d.len) break;
    if (!(d.flags & kVringDescFNext)) return -1;
    offset -= d.len;
    idx = d.next;
    if (GetDesc(v, q, idx, &d)) return -1;
  }
  for (;;) {
    int l = len < (int)d.len - offset ? len : (int)d.len - offset;
    if (to_guest ? WritePhys(v->bus, d.addr + offset, p, l)
                 : ReadPhys(v->bus, d.addr + offset, p, l))
      return -1;
    len -= l;
    if (len == 0) break;
    offset += l;
    p += l;
    if (offset == (int)d.len) {
      if (!(d.flags & kVringDescFNext)) return -1;
      idx = d.next;
      if (GetDesc(v, q, idx, &d)) return -1;
      if ((d.flags & kVringDescFWrite) != (uint16_t)want_write) return -1;
      offset = 0;
    }
  }
  return 0;
}

int VirtioCopyFromChain(VirtioDevice* v, int q, int idx, int offset, void* buf,
                        int len) {
  return CopyChain(v, q, idx, offset, buf, len, 0);
}

int VirtioCopyToChain(VirtioDevice* v, int q, int idx, int offset, const void* buf,
                      int len) {
  return CopyChain(v, q, idx, offset, (void*)buf, len, 1);
}

void VirtioConsumeChain(VirtioDevice* v, int q, int idx, int written) {
  VirtioQueue* qs = &v->queue[q];
  uint64_t used_idx_addr = qs->used_addr + 2;
  uint16_t used_idx = ReadPhys16(v->bus, used_idx_addr);
  uint64_t slot = qs->used_addr + 4 + (uint64_t)(used_idx & (qs->num - 1)) * 8;
  WritePhys32(v->bus, slot, (uint32_t)idx);
  WritePhys32(v->bus, slot + 4, (uint32_t)written);
  WritePhys16(v->bus, used_idx_addr, used_idx + 1);
  v->int_status |= kVirtioIntUsedRing;
  if (v->set_irq) v->set_irq(v->irq_ctx, v->irq_src, 1);
}

void VirtioNotifyQueue(VirtioDevice* v, int q) {
  VirtioQueue* qs = &v->queue[q];
  int rd, wr;
  if (qs->manual) return;
  if (qs->num == 0) {
    DebugVirtio("kick-unconfigured", v->device_id, (uint64_t)q, 0, 0);
    return;
  }
  uint16_t avail_idx = ReadPhys16(v->bus, qs->avail_addr + 2);
  // One row per drain attempt: the address the transport scans and the two
  // indices it compares. A guest stuck in virtqueue_get_buf with a published
  // avail->idx shows up here as a zero read at the scanned address.
  DebugVirtio("drain", v->device_id, (uint64_t)q, qs->avail_addr,
              (uint64_t)avail_idx << 16 | qs->last_avail_idx);
  while (qs->last_avail_idx != avail_idx) {
    int idx = ReadPhys16(v->bus, qs->avail_addr + 4 +
                                      (uint64_t)(qs->last_avail_idx & (qs->num - 1)) * 2);
    if (ChainSizes(v, q, idx, &rd, &wr)) {
      DebugVirtio("chainfail", v->device_id, (uint64_t)q, qs->desc_addr, qs->last_avail_idx);
      qs->last_avail_idx++;
      continue;
    }
    if (v->recv(v, q, idx, rd, wr)) break;
    qs->last_avail_idx++;
    avail_idx = ReadPhys16(v->bus, qs->avail_addr + 2);
  }
}

uint16_t VirtioQueueAvail(VirtioDevice* v, int q) {
  VirtioQueue* qs = &v->queue[q];
  if (qs->num == 0) return 0;
  return ReadPhys16(v->bus, qs->avail_addr + 2);
}

int VirtioManualChain(VirtioDevice* v, int q) {
  VirtioQueue* qs = &v->queue[q];
  if (qs->num == 0) return -1;
  uint16_t avail_idx = ReadPhys16(v->bus, qs->avail_addr + 2);
  if (qs->last_avail_idx == avail_idx) return -1;
  return ReadPhys16(v->bus,
                    qs->avail_addr + 4 + (uint64_t)(qs->last_avail_idx & (qs->num - 1)) * 2);
}

// Fill a manual chain with host bytes and retire it; the caller polls
// VirtioManualChain first. Returns the byte count delivered (0 = nothing).
int VirtioManualFill(VirtioDevice* v, int q, const void* buf, int len) {
  VirtioQueue* qs = &v->queue[q];
  int idx = VirtioManualChain(v, q);
  if (idx < 0) return 0;
  if (VirtioCopyToChain(v, q, idx, 0, buf, len)) return 0;
  VirtioConsumeChain(v, q, idx, len);
  qs->last_avail_idx++;
  return len;
}

// ---- transport reset (spec 2.4.1: status 0 write) ----

static void TransportReset(VirtioDevice* v) {
  v->guest_features = 0;
  v->device_features_sel = 0;
  v->driver_features_sel = 0;
  v->queue_sel = 0;
  v->int_status = 0;
  v->page_shift = 12;
  v->align = 0x1000;
  for (int i = 0; i < kVirtioMaxQueue; i++) {
    VirtioQueue* qs = &v->queue[i];
    qs->num = 0;
    qs->pfn = 0;
    qs->desc_addr = qs->avail_addr = qs->used_addr = 0;
    qs->last_avail_idx = 0;
    qs->ready = 0;
  }
  if (v->reset_hook) v->reset_hook(v);
}

void VirtioInit(VirtioDevice* v, Bus* bus, uint32_t device_id, uint32_t vendor_id,
                uint64_t features, uint32_t version, int config_size,
                VirtioRecvFn recv) {
  memset(v, 0, sizeof *v);
  v->bus = bus;
  v->device_id = device_id;
  v->vendor_id = vendor_id;
  v->device_features = features;
  v->version = version;
  v->config_size = config_size;
  v->recv = recv;
  TransportReset(v);
}

// ---- register file ----

// One 32-bit word of the 64-bit feature space, selected by the guest's
// DeviceFeaturesSel/DriverFeaturesSel (virtio-mmio spec 4.2.2).
static uint32_t FeatureWord(uint64_t features, uint32_t sel) {
  return sel < 2 ? (uint32_t)(features >> (32 * sel)) : 0;
}

static uint64_t MmioRead(void* dev, uint64_t addr, int size) {
  VirtioDevice* v = dev;
  // DeviceOps receive the bus's absolute address; this transport owns one
  // 4KB-aligned slot window (spec 4.2.2), whose low 12 bits are the register
  // offset (the UART driver's addr & 7 is the same convention).
  addr &= 0xfff;
  if (addr >= kVirtioMmioConfig) {
    // config space: byte-granular (the console cols/rows fields are read a
    // byte at a time by guests)
    uint64_t off = addr - kVirtioMmioConfig;
    if (off + (uint64_t)size > (uint64_t)v->config_size) return 0;
    uint64_t val = 0;
    memcpy(&val, v->config + off, (size_t)size);
    return val;
  }
  if (size != 4) return 0;
  VirtioQueue* qs = &v->queue[v->queue_sel];
  switch (addr) {
    case kVirtioMmioMagic:
      return kVirtioMagicValue;
    case kVirtioMmioVersion:
      return v->version;
    case kVirtioMmioDeviceId:
      return v->device_id;
    case kVirtioMmioVendorId:
      return v->vendor_id;
    case kVirtioMmioDeviceFeatures:
      return FeatureWord(v->device_features, v->device_features_sel);
    case kVirtioMmioDeviceFeaturesSel:
      return v->device_features_sel;
    case kVirtioMmioDriverFeatures:
      return FeatureWord(v->guest_features, v->driver_features_sel);
    case kVirtioMmioDriverFeaturesSel:
      return v->driver_features_sel;
    case kVirtioMmioQueueSel:
      return v->queue_sel;
    case kVirtioMmioQueueNumMax:
      return kVirtioMaxQueueNum;
    case kVirtioMmioQueueNum:
      return qs->num;
    case kVirtioMmioQueuePfn:
      return qs->pfn;
    case kVirtioMmioQueueReady:
      return (uint64_t)qs->ready;
    case kVirtioMmioQueueDescLow:
      return (uint32_t)qs->desc_addr;
    case kVirtioMmioQueueDescHigh:
      return (uint32_t)(qs->desc_addr >> 32);
    case kVirtioMmioQueueDriverLow:
      return (uint32_t)qs->avail_addr;
    case kVirtioMmioQueueDriverHigh:
      return (uint32_t)(qs->avail_addr >> 32);
    case kVirtioMmioQueueDeviceLow:
      return (uint32_t)qs->used_addr;
    case kVirtioMmioQueueDeviceHigh:
      return (uint32_t)(qs->used_addr >> 32);
    case kVirtioMmioConfigGeneration:
      return 0;
    case kVirtioMmioInterruptStatus:
      DebugVirtio("isr", v->device_id, v->int_status, v->queue_sel, 0);
      return v->int_status;
    case kVirtioMmioStatus:
      return v->status;
    default:
      return 0;
  }
}

static void SetAddrWord(uint64_t* a, uint32_t val, int high) {
  if (high)
    *a = (*a & 0xffffffffULL) | ((uint64_t)val << 32);
  else
    *a = (*a & ~0xffffffffULL) | val;
}

static void MmioWrite(void* dev, uint64_t addr, int size, uint64_t val64) {
  VirtioDevice* v = dev;
  uint32_t val = (uint32_t)val64;
  addr &= 0xfff;  // slot-relative register offset, see MmioRead
  if (addr >= kVirtioMmioConfig) {
    uint64_t off = addr - kVirtioMmioConfig;
    if (off + (uint64_t)size <= (uint64_t)v->config_size) {
      memcpy(v->config + off, &val, (size_t)size);
      if (v->config_hook) v->config_hook(v);
    }
    return;
  }
  if (size != 4) return;
  VirtioQueue* qs = &v->queue[v->queue_sel];
  switch (addr) {
    case kVirtioMmioDeviceFeaturesSel:
      v->device_features_sel = val;
      break;
    case kVirtioMmioDriverFeatures:
      if (v->driver_features_sel < 2) {
        int shift = 32 * (int)v->driver_features_sel;
        v->guest_features =
            (v->guest_features & ~(0xffffffffULL << shift)) | ((uint64_t)val << shift);
      }
      DebugVirtio("feat", v->device_id, v->driver_features_sel, val, v->device_features);
      break;
    case kVirtioMmioDriverFeaturesSel:
      v->driver_features_sel = val;
      break;
    case kVirtioMmioGuestPageSize:
      if (v->version == 1) {
        uint64_t sz = val;
        int shift = 12;
        while ((1ull << shift) < sz) shift++;
        v->page_shift = shift;
      }
      break;
    case kVirtioMmioQueueSel:
      DebugVirtio("qsel", v->device_id, val, v->queue_sel, 0);
      if (val < kVirtioMaxQueue) v->queue_sel = val;
      break;
    case kVirtioMmioQueueNum:
      DebugVirtio("qnum", v->device_id, v->queue_sel, val, qs->num);
      if (val > 0 && (val & (val - 1)) == 0 && val <= kVirtioMaxQueueNum) qs->num = val;
      break;
    case kVirtioMmioQueueAlign:
      if (v->version == 1) v->align = val < 0x1000 ? 0x1000 : val;
      break;
    case kVirtioMmioQueuePfn:
      if (v->version == 1) {
        // vring_init() addressing: desc at pfn<<page_shift, avail follows the
        // descriptors, used is aligned past the avail ring and its used_event.
        uint64_t desc = (uint64_t)val << v->page_shift;
        uint64_t avail = desc + (uint64_t)qs->num * sizeof(VringDesc);
        uint64_t used =
            (avail + 6 + (uint64_t)qs->num * 2 + v->align - 1) & ~(uint64_t)(v->align - 1);
        qs->pfn = val;
        qs->desc_addr = desc;
        qs->avail_addr = avail;
        qs->used_addr = used;
        qs->last_avail_idx = 0;
        DebugVirtio("qpfn", v->device_id, v->queue_sel, val, avail);
      }
      break;
    case kVirtioMmioQueueReady:
      if (v->version == 2) {
        int was = qs->ready;
        qs->ready = (int)(val & 1);
        if (!was && qs->ready) qs->last_avail_idx = 0;
      }
      break;
    case kVirtioMmioQueueDescLow:
      if (v->version == 2) SetAddrWord(&qs->desc_addr, val, 0);
      break;
    case kVirtioMmioQueueDescHigh:
      if (v->version == 2) SetAddrWord(&qs->desc_addr, val, 1);
      break;
    case kVirtioMmioQueueDriverLow:
      if (v->version == 2) SetAddrWord(&qs->avail_addr, val, 0);
      break;
    case kVirtioMmioQueueDriverHigh:
      if (v->version == 2) SetAddrWord(&qs->avail_addr, val, 1);
      break;
    case kVirtioMmioQueueDeviceLow:
      if (v->version == 2) SetAddrWord(&qs->used_addr, val, 0);
      break;
    case kVirtioMmioQueueDeviceHigh:
      if (v->version == 2) SetAddrWord(&qs->used_addr, val, 1);
      break;
    case kVirtioMmioQueueNotify:
      DebugVirtio("notify-mmio", v->device_id, val, addr, 0);
      if (val < kVirtioMaxQueue) VirtioNotifyQueue(v, (int)val);
      break;
    case kVirtioMmioInterruptAck:
      DebugVirtio("ack", v->device_id, val, v->int_status, 0);
      v->int_status &= ~val;
      if (v->int_status == 0 && v->set_irq) v->set_irq(v->irq_ctx, v->irq_src, 0);
      break;
    case kVirtioMmioStatus:
      DebugVirtio("status", v->device_id, val, v->status, 0);
      v->status = val;
      if (val == 0) {
        if (v->set_irq) v->set_irq(v->irq_ctx, v->irq_src, 0);
        TransportReset(v);
      }
      break;
    default:
      break;
  }
}

const DeviceOps kVirtioMmioOps = {"virtio_mmio", MmioRead, MmioWrite};

// A slot the machine advertises but nothing is attached to. QEMU
// hw/virtio/virtio-mmio.c keeps the magic, version and vendor ID readable
// when no backend is bound and treats every other register as read-as-zero,
// write-ignored: the driver's probe passes the identity checks, reads
// DeviceID 0 and no driver claims the slot. Leaving the address unmapped
// instead turns the probe's first register read into an access fault.
static uint64_t MmioEmptyRead(void* dev, uint64_t addr, int size) {
  (void)dev;
  (void)size;
  switch (addr & 0xfff) {
    case kVirtioMmioMagic:
      return kVirtioMagicValue;
    case kVirtioMmioVersion:
      return 1;  // legacy transport, like the attached slots
    case kVirtioMmioVendorId:
      return kVirtioVendorId;
    default:
      return 0;  // DeviceID included: zero means "no backend"
  }
}

static void MmioEmptyWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)dev;
  (void)addr;
  (void)size;
  (void)val;
}

const DeviceOps kVirtioMmioEmptyOps = {"virtio_mmio", MmioEmptyRead, MmioEmptyWrite};
