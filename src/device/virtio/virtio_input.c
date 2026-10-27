// VirtIO input (spec 5.10). The selector-driven config page, the event
// bitmaps and the ABS ranges mirror dearchap-tinyemu virtio.c
// virtio_input_config_write (which this guest's kernel is calibrated
// against); event codes are Linux input-event-codes.h. Events go out one per
// driver-posted buffer through the transport's manual queue, exactly like
// the console's receive path.

#include <stdlib.h>
#include <string.h>

#include "device/virtio/virtio_input.h"
#include "util/log.h"

// config selects (spec 5.10.2)
enum {
  kInputCfgUnset = 0x00,
  kInputCfgIdName = 0x01,
  kInputCfgIdSerial = 0x02,
  kInputCfgIdDevids = 0x03,
  kInputCfgPropBits = 0x10,
  kInputCfgEvBits = 0x11,
  kInputCfgAbsInfo = 0x12,
};

// event types / codes (input-event-codes.h)
enum {
  kEvSyn = 0x00,
  kEvKey = 0x01,
  kEvRel = 0x02,
  kEvAbs = 0x03,
  kEvRep = 0x14,
};
enum {
  kRelWheel = 0x08,
  kAbsX = 0x00,
  kAbsY = 0x01,
};
enum {
  kBtnLeft = 0x110,
  kBtnRight = 0x111,
  kBtnMiddle = 0x112,
};
static const uint16_t kButtons[] = {kBtnLeft, kBtnRight, kBtnMiddle};

enum { kAbsScale = 0x8000 };  // absolute range 0..0x7fff, tinyemu's scale

enum { kQueueEvent = 0, kQueueStatus = 1 };

static void SetBit(uint8_t* tab, int k) { tab[k >> 3] |= (uint8_t)(1 << (k & 7)); }

// The driver wrote select/subsel: answer with size + payload at offsets 2/8.
static void InputConfigSelected(VirtioDevice* v) {
  VirtioInput* in = (VirtioInput*)v;
  uint8_t* cfg = v->config;
  cfg[2] = 0;
  memset(cfg + 8, 0, 128);
  switch (cfg[0]) {
    case kInputCfgIdName: {
      const char* name = in->type == kVirtioInputKeyboard ? "cemu virtio keyboard"
                                                          : "cemu virtio tablet";
      cfg[2] = (uint8_t)strlen(name);
      memcpy(cfg + 8, name, cfg[2]);
      break;
    }
    case kInputCfgEvBits: {
      if (in->type == kVirtioInputKeyboard) {
        if (cfg[1] == kEvKey) {  // keys 0..127 all present, tinyemu's answer
          cfg[2] = 128 / 8;
          memset(cfg + 8, 0xff, 128 / 8);
        } else if (cfg[1] == kEvRep) {
          cfg[2] = 1;
        }
      } else {
        if (cfg[1] == kEvKey) {
          cfg[2] = 512 / 8;
          for (size_t i = 0; i < sizeof kButtons / sizeof kButtons[0]; i++)
            SetBit(cfg + 8, kButtons[i]);
        } else if (cfg[1] == kEvRel) {
          cfg[2] = 2;
          SetBit(cfg + 8, kRelWheel);
        } else if (cfg[1] == kEvAbs) {
          cfg[2] = 1;
          SetBit(cfg + 8, kAbsX);
          SetBit(cfg + 8, kAbsY);
        }
      }
      break;
    }
    case kInputCfgAbsInfo:
      if (in->type == kVirtioInputTablet && (cfg[1] == kAbsX || cfg[1] == kAbsY)) {
        cfg[2] = 5 * 4;
        uint32_t range[5] = {0, kAbsScale - 1, 0, 0, 0};
        memcpy(cfg + 8, range, sizeof range);
      }
      break;
    default:
      break;  // serial/devids/propbits: empty answer
  }
}

// Queue 1: the driver reporting LED state. Acknowledge and drop.
static int InputRecv(VirtioDevice* v, int q, int desc, int rd, int wr) {
  (void)rd;
  (void)wr;
  if (q == kQueueStatus) VirtioConsumeChain(v, q, desc, 0);
  return 0;
}

static void Enqueue(VirtioInput* in, uint16_t type, uint16_t code, int32_t value) {
  if (in->pending_count >= kVirtioInputMaxPending) return;  // drop when flooded
  int tail = (in->pending_head + in->pending_count) % kVirtioInputMaxPending;
  in->pending[tail].type = type;
  in->pending[tail].code = code;
  in->pending[tail].value = value;
  in->pending_count++;
}

void VirtioInputPoll(VirtioInput* in) {
  VirtioDevice* v = &in->vdev;
  while (in->pending_count > 0) {
    uint8_t ev[8];
    memcpy(ev, &in->pending[in->pending_head].type, 2);
    memcpy(ev + 2, &in->pending[in->pending_head].code, 2);
    memcpy(ev + 4, &in->pending[in->pending_head].value, 4);
    if (VirtioManualFill(v, kQueueEvent, ev, sizeof ev) == 0) break;
    in->pending_head = (in->pending_head + 1) % kVirtioInputMaxPending;
    in->pending_count--;
  }
}

void VirtioInputKey(VirtioInput* in, int linux_key, int down) {
  Enqueue(in, kEvKey, (uint16_t)linux_key, down);
  Enqueue(in, kEvSyn, 0, 0);
}

void VirtioInputPointer(VirtioInput* in, int abs_x, int abs_y) {
  if (abs_x < 0) abs_x = 0;
  if (abs_y < 0) abs_y = 0;
  if (abs_x >= kAbsScale) abs_x = kAbsScale - 1;
  if (abs_y >= kAbsScale) abs_y = kAbsScale - 1;
  Enqueue(in, kEvAbs, kAbsX, abs_x);
  Enqueue(in, kEvAbs, kAbsY, abs_y);
  // evdev buffers events until SYN_REPORT; without it the motion sits in the
  // kernel until the next button/key packet flushes the backlog.
  Enqueue(in, kEvSyn, 0, 0);
}

void VirtioInputWheel(VirtioInput* in, int notches) {
  Enqueue(in, kEvRel, kRelWheel, notches);
  Enqueue(in, kEvSyn, 0, 0);
}

void VirtioInputButton(VirtioInput* in, int bit, int down) {
  if (bit < 0 || bit > 2) return;
  uint32_t mask = 1u << bit;
  if (down)
    in->buttons |= mask;
  else
    in->buttons &= ~mask;
  Enqueue(in, kEvKey, kButtons[bit], down);
  Enqueue(in, kEvSyn, 0, 0);
}

VirtioInput* VirtioInputCreate(Bus* bus, uint64_t base, uint64_t size,
                               VirtioInputType type, int irq_src,
                               void (*set_irq)(void*, int, int), void* irq_ctx) {
  VirtioInput* in = calloc(1, sizeof *in);
  VirtioInit(&in->vdev, bus, 18 /*virtio-input id, spec 5.10*/, kVirtioVendorId,
             1ULL << kVirtioFVersion1 /* Linux's virtio_input probe gate */,
             2 /*modern transport*/,
             256 /*selector-driven config page*/, InputRecv);
  in->type = type;
  in->vdev.config_hook = InputConfigSelected;
  in->vdev.queue[kQueueEvent].manual = 1;
  in->vdev.irq_src = irq_src;
  in->vdev.set_irq = set_irq;
  in->vdev.irq_ctx = irq_ctx;
  BusAddRegion(bus, base, size, &kVirtioMmioOps, &in->vdev);
  return in;
}

void VirtioInputDestroy(VirtioInput* in) { free(in); }
