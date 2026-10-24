#include "device/video/ramfb.h"

#include <stdlib.h>
#include <string.h>

enum { kRamfbPixelsPerWord = 32 };

static uint64_t RamfbRead(void* dev, uint64_t addr, int size) {
  RamfbDevice* d = (RamfbDevice*)dev;
  uint64_t off = addr - d->base;
  uint64_t v = 0;
  for (int i = 0; i < size; i++) v |= (uint64_t)d->mem[off + i] << (8 * i);
  return v;
}

static void RamfbWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  RamfbDevice* d = (RamfbDevice*)dev;
  uint64_t off = addr - d->base;
  for (int i = 0; i < size; i++) d->mem[off + i] = (uint8_t)(val >> (8 * i));
  d->dirty = 1;
  d->version++;
}

const DeviceOps kRamfbOps = {"ramfb", RamfbRead, RamfbWrite};

// ---- the host-side picture (device/video/display.h) --------------------------

static int RamfbWidth(void* dev) { return ((RamfbDevice*)dev)->width; }
static int RamfbHeight(void* dev) { return ((RamfbDevice*)dev)->height; }

static const uint32_t* RamfbFramebuffer(void* dev) {
  RamfbDevice* d = (RamfbDevice*)dev;
  if (d->dirty) {
    for (int row = 0; row < d->height; row++) {
      // Word line 0 is the bottom line on a bottom-up raster.
      int line = d->bottom_up ? d->height - 1 - row : row;
      const uint8_t* src = d->mem + (size_t)line * d->words_per_line * 4;
      uint32_t* dst = d->px + (size_t)row * d->width;
      for (int w = 0; w < d->words_per_line; w++) {
        uint32_t word = (uint32_t)src[w * 4] | (uint32_t)src[w * 4 + 1] << 8 |
                        (uint32_t)src[w * 4 + 2] << 16 | (uint32_t)src[w * 4 + 3] << 24;
        for (int b = 0; b < kRamfbPixelsPerWord; b++) {
          int bit = d->lsb_first ? b : kRamfbPixelsPerWord - 1 - b;
          dst[b] = (word >> bit) & 1 ? d->on : d->off;
        }
        dst += kRamfbPixelsPerWord;
      }
    }
    d->dirty = 0;
  }
  return d->px;
}

static uint32_t RamfbVersion(void* dev) { return ((RamfbDevice*)dev)->version; }

const DisplaySourceOps kRamfbDisplayOps = {RamfbWidth, RamfbHeight, RamfbFramebuffer,
                                           RamfbVersion};

// ---- lifecycle ----------------------------------------------------------------

int RamfbInit(RamfbDevice* d, int width, int height, int lsb_first, int bottom_up,
              uint32_t on, uint32_t off) {
  memset(d, 0, sizeof(*d));
  if (width <= 0 || height <= 0) return -1;
  d->width = width;
  d->height = height;
  d->words_per_line = (width + kRamfbPixelsPerWord - 1) / kRamfbPixelsPerWord;
  d->lsb_first = lsb_first;
  d->bottom_up = bottom_up;
  d->on = on;
  d->off = off;
  size_t win = (size_t)d->words_per_line * 4 * height;
  d->mem = (uint8_t*)calloc(1, win);
  d->px = (uint32_t*)calloc((size_t)width * height, sizeof(uint32_t));
  if (!d->mem || !d->px) {
    RamfbDestroy(d);
    return -1;
  }
  d->dirty = 1;  // the blank frame still has to be rendered once
  return 0;
}

void RamfbRegister(Bus* bus, RamfbDevice* d, uint64_t base) {
  d->base = base;
  BusAddRegion(bus, base, (uint64_t)d->words_per_line * 4 * d->height, &kRamfbOps, d);
}

void RamfbDestroy(RamfbDevice* d) {
  free(d->mem);
  free(d->px);
  d->mem = NULL;
  d->px = NULL;
}
