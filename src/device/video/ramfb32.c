// The colour simple-framebuffer display source: reads the guest's scanout
// straight out of DRAM (the DT "framebuffer" binding's contract -- the
// bootloader pointed Linux at a fixed physical address, Linux/X draw there,
// nobody notifies anybody) and publishes it through the display vtable.
// a8r8g8b8 is 32-bit little-endian with bytes B,G,R,A, the byte order the
// simplefb driver and X's fbdev both write; the render buffer is XRGB.

#include <stdlib.h>
#include <string.h>

#include "debug/debug.h"
#include "device/video/ramfb32.h"
#include "host/host.h"

enum { kMaxFps = 30 };  // scanout check rate; a repaint needs a change anyway

static int Width(void* dev) { return ((Ramfb32Device*)dev)->width; }
static int Height(void* dev) { return ((Ramfb32Device*)dev)->height; }
static const uint32_t* Framebuffer(void* dev) { return ((Ramfb32Device*)dev)->px; }
static uint32_t Version(void* dev) { return ((Ramfb32Device*)dev)->version; }

const DisplaySourceOps kRamfb32DisplayOps = {Width, Height, Framebuffer, Version};

int Ramfb32Init(Ramfb32Device* d, Bus* bus, uint64_t base, int width, int height,
                uint64_t stride) {
  memset(d, 0, sizeof *d);
  d->bus = bus;
  d->base = base;
  d->width = width;
  d->height = height;
  d->stride = stride;
  d->px = malloc((size_t)width * height * 4);
  d->shadow_size = stride * (uint64_t)height;
  d->shadow = malloc((size_t)d->shadow_size);
  if (!d->px || !d->shadow) {
    free(d->px);
    free(d->shadow);
    return -1;
  }
  memset(d->px, 0, (size_t)width * height * 4);
  return 0;
}

void Ramfb32Poll(Ramfb32Device* d) {
  int64_t now = HostTimerNow();
  if (now - d->last_poll_us < 1000000 / kMaxFps) return;
  d->last_poll_us = now;

  // Compare through the bus page by page: the scanout is contiguous RAM,
  // but each 4 KiB page is mapped separately.
  uint64_t checked = 0;
  int changed = 0;
  uint8_t* shadow = d->shadow;
  while (checked < d->shadow_size) {
    uint64_t len = 0x1000 - ((d->base + checked) & 0xfff);
    if (len > d->shadow_size - checked) len = d->shadow_size - checked;
    uint8_t* host;
    if (BusRamRange(d->bus, d->base + checked, len, &host) != 0) return;  // not RAM
    if (memcmp(host, shadow + checked, len) != 0) {
      memcpy(shadow + checked, host, len);
      changed = 1;
    }
    checked += len;
  }
  if (!changed) return;
  if (DebugOn(kDbgMark)) DebugMark("ramfb32", (int)d->version, changed);

  for (int y = 0; y < d->height; y++) {
    const uint8_t* src = shadow + (size_t)y * d->stride;
    uint32_t* dst = d->px + (size_t)y * d->width;
    for (int x = 0; x < d->width; x++) {
      dst[x] = 0xff000000u | (uint32_t)src[x * 4 + 2] << 16 |
               (uint32_t)src[x * 4 + 1] << 8 | src[x * 4];
    }
  }
  d->version++;
}

void Ramfb32Destroy(Ramfb32Device* d) {
  free(d->px);
  free(d->shadow);
}
