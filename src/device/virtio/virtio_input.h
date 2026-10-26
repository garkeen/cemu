#ifndef CEMU_DEVICE_VIRTIO_VIRTIO_INPUT_H
#define CEMU_DEVICE_VIRTIO_VIRTIO_INPUT_H

#include <stdint.h>

#include "bus/bus.h"
#include "device/virtio/virtio.h"

// VirtIO input (spec 5.10): an evdev device on a virtqueue. Queue 0 carries
// events device->driver (the driver posts writable buffers; each delivered
// event is one struct virtio_input_event {u16 type, u16 code, s32 value} and
// retires one buffer with a used-ring interrupt), queue 1 carries LED status
// driver->device (acknowledged and dropped). The config page is
// selector-driven: the driver writes select/subsel at offsets 0/1, the
// device answers with a size at offset 2 and the payload at offset 8 --
// device name, event bitmaps, absolute-axis ranges (spec 5.10.2).
//
// Host events are queued here and pushed into posted buffers by Poll; a
// keyboard instance takes Linux KEY_* codes, a tablet absolute X/Y in
// 0..0x7fff plus buttons. Reference: dearchap-tinyemu virtio.c
// virtio_input_*, event codes from Linux include/uapi/linux/input-event-codes.h.

enum { kVirtioInputMaxPending = 64 };

typedef enum { kVirtioInputKeyboard, kVirtioInputTablet } VirtioInputType;

typedef struct VirtioInput {
  VirtioDevice vdev;
  VirtioInputType type;
  uint32_t buttons;  // bit 0 left, 1 right, 2 middle
  struct {
    uint16_t type, code;
    int32_t value;
  } pending[kVirtioInputMaxPending];
  int pending_head, pending_count;
} VirtioInput;

VirtioInput* VirtioInputCreate(Bus* bus, uint64_t base, uint64_t size,
                               VirtioInputType type, int irq_src,
                               void (*set_irq)(void*, int, int), void* irq_ctx);
// Keyboard: Linux KEY_* code, down/up.
void VirtioInputKey(VirtioInput* in, int linux_key, int down);
// Tablet: 0..0x7fff absolute coordinates (0x7fff = right/bottom edge).
void VirtioInputPointer(VirtioInput* in, int abs_x, int abs_y);
// Wheel notch (REL_WHEEL); buttons by bit 0/1/2.
void VirtioInputWheel(VirtioInput* in, int notches);
void VirtioInputButton(VirtioInput* in, int bit, int down);
// Deliver queued events into buffers the driver posted; board poll calls it.
void VirtioInputPoll(VirtioInput* in);
void VirtioInputDestroy(VirtioInput* in);

#endif
