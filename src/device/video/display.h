#ifndef CEMU_DEVICE_VIDEO_DISPLAY_H
#define CEMU_DEVICE_VIDEO_DISPLAY_H

#include <stdint.h>

// The board-side half of the display channel (阶段 3.5 片 2): a device that
// owns guest-visible video state renders into a pixel buffer and publishes
// it through this vtable; main.c attaches a host window (-display) to the
// board's display_ops and the run loop pumps that window. The host side
// (host/display_win.c) stays device-agnostic: it sees only a fixed XRGB
// buffer plus a change counter, so the CGA today and VGA graphics / riscv
// ramfb later all ride the same seam.
typedef struct DisplaySourceOps {
  int width;   // framebuffer size in pixels, fixed for the device's lifetime
  int height;
  // Returns the XRGB (0xffRRGGBB, little-endian = B,G,R,255 bytes) pixel
  // buffer. Never reallocates: the host window keeps the pointer.
  const uint32_t* (*Framebuffer)(void* dev);
  // Change counter of the framebuffer contents; the window repaints when it
  // differs from what it last showed.
  uint32_t (*Version)(void* dev);
} DisplaySourceOps;

#endif
