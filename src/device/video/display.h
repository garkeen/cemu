#ifndef CEMU_DEVICE_VIDEO_DISPLAY_H
#define CEMU_DEVICE_VIDEO_DISPLAY_H

#include <stdint.h>

// The board-side half of the display channel (阶段 3.5 片 2): a device that
// owns guest-visible video state renders into a pixel buffer and publishes
// it through this vtable; main.c attaches a host window (-display) to the
// board's display_ops and the run loop pumps that window. The host side
// (host/display_win.c) stays device-agnostic: it sees only the vtable, so the
// VGA family core today and riscv ramfb later all ride the same seam.
typedef struct DisplaySourceOps {
  // The picture's size in pixels *now*. A card whose mode can change reports
  // the current mode's size, and the host window follows it (that is why these
  // are calls, not constants: the VGA family core's mode set changes both).
  int (*Width)(void* dev);
  int (*Height)(void* dev);
  // Returns the XRGB (0xffRRGGBB, little-endian = B,G,R,255 bytes) pixel
  // buffer for the current picture. Never reallocates: the host window keeps
  // the pointer.
  const uint32_t* (*Framebuffer)(void* dev);
  // Change counter of the framebuffer contents; the window repaints when it
  // differs from what it last showed.
  uint32_t (*Version)(void* dev);
} DisplaySourceOps;

#endif
