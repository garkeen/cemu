#ifndef CEMU_DEVICE_VIDEO_RAMFB_H
#define CEMU_DEVICE_VIDEO_RAMFB_H

#include <stdint.h>

#include "bus/bus.h"
#include "device/video/display.h"

// A monochrome linear framebuffer that lives in guest RAM: the device owns
// one window of the address space (the way a video aperture does) and
// publishes its picture through the DisplaySourceOps seam. Boards for guests
// that draw straight into RAM wire one of these instead of a programmed video
// card; nothing here knows a guest or a mode protocol.
//
// Geometry and bit order are creation parameters:
//   - one bit per pixel, packed 32 bits per word, words little-endian;
//     `lsb_first` set = bit 0 of a word is the word's leftmost pixel
//     (cleared = bit 31 is),
//   - `bottom_up` set = word line 0 is the screen's bottom line (a raster
//     that scans the frame from low addresses upward),
//   - `on`/`off` are the XRGB colors of a set and a clear bit.
// Stores bump the version counter, which is all the host window needs: it
// repaints when Version changes and pulls Framebuffer, which re-renders
// lazily. There is no per-line damage list — a repaint renders the frame.
typedef struct RamfbDevice {
  uint64_t base;  // window base, filled by RamfbRegister
  int width, height;
  int words_per_line;  // 32 pixels per word
  int lsb_first, bottom_up;
  uint32_t on, off;
  uint8_t* mem;  // the window's backing store (guest-visible bytes)
  uint32_t* px;  // width*height XRGB render buffer
  uint32_t version;
  int dirty;
} RamfbDevice;

// Returns 0, or -1 when the render buffer cannot be allocated. The window is
// words_per_line*4*height bytes, where words_per_line = (width+31)/32.
int RamfbInit(RamfbDevice* d, int width, int height, int lsb_first, int bottom_up,
              uint32_t on, uint32_t off);
void RamfbRegister(Bus* bus, RamfbDevice* d, uint64_t base);
void RamfbDestroy(RamfbDevice* d);

extern const DeviceOps kRamfbOps;                // the guest-side window
extern const DisplaySourceOps kRamfbDisplayOps;  // the host-side picture

#endif  // CEMU_DEVICE_VIDEO_RAMFB_H
