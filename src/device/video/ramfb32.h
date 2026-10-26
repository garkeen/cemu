#ifndef CEMU_DEVICE_VIDEO_RAMFB32_H
#define CEMU_DEVICE_VIDEO_RAMFB32_H

#include <stdint.h>

#include "bus/bus.h"
#include "device/video/display.h"

// A colour linear framebuffer that lives in plain guest RAM -- the
// simple-framebuffer a device tree hands Linux (the "framebuffer" DT binding:
// the bootloader owns a scanout buffer at a fixed physical address and the
// OS just draws there). Unlike the monochrome ramfb this device owns no
// window of its own: the guest writes main memory and the device reads it
// through the bus. Poll converts the a8r8g8b8 (32-bit LE, BGRA bytes)
// scanout into the XRGB render buffer at a capped frame rate and bumps the
// version only when the picture actually changed, so the host window
// repaints exactly when there is something new to show.
typedef struct Ramfb32Device {
  Bus* bus;
  uint64_t base;
  int width, height;
  uint64_t stride;  // bytes per line
  uint32_t* px;     // width*height XRGB render buffer (display vtable)
  uint8_t* shadow;  // last scanout seen, to detect change
  uint64_t shadow_size;
  uint32_t version;
  int64_t last_poll_us;
} Ramfb32Device;

// Returns 0, or -1 when the buffers cannot be allocated.
int Ramfb32Init(Ramfb32Device* d, Bus* bus, uint64_t base, int width, int height,
                uint64_t stride);
// Frame-rate-capped scanout refresh; the board's poll calls it.
void Ramfb32Poll(Ramfb32Device* d);
void Ramfb32Destroy(Ramfb32Device* d);

extern const DisplaySourceOps kRamfb32DisplayOps;

#endif  // CEMU_DEVICE_VIDEO_RAMFB32_H
