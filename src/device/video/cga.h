#ifndef CEMU_DEVICE_VIDEO_CGA_H
#define CEMU_DEVICE_VIDEO_CGA_H

#include <stdint.h>

#include "bus/bus.h"
#include "device/video/display.h"
#include "mem/ram.h"

// IBM Color/Graphics Adapter (阶段 3.5 片 2): the 1981 PC display card — a
// 16KB frame buffer at 0xB8000, an MC6845 CRTC addressed through 0x3D0-0x3D5,
// a mode-control latch at 0x3D8, a color-select latch at 0x3D9, the status
// port at 0x3DA and the light-pen strobes at 0x3DB/0x3DC (IBM CGA Technical
// Reference). This slice renders the alphanumeric modes 0-3; the graphics
// modes are registered in AGENTS.md 简化登记. Raster status timing is driven
// from the host clock in CgaPoll — the same hook the run loop uses for the
// PIT — and the rendered output is published through kCgaDisplayOps.
enum {
  kCgaVramAddr = 0xB8000,
  kCgaVramSize = 0x4000,  // 16KB: 8192 words; text pages are 2KB each
  kCgaFbWidth = 640,      // 80 cells x 8 dots, scanlines doubled below
  kCgaFbHeight = 400,     // 25 cells x 8 scanlines, each line drawn twice
};

typedef struct CgaDevice {
  RamDevice vram_ram;  // kRamOps identity for the 0xB8000 window
  uint8_t vram[kCgaVramSize];
  uint8_t crtc[18];   // MC6845 R0-R17 (the chip has no more registers)
  uint8_t crtc_index;  // 5-bit index latch; ports 0x3D0/0x3D2/0x3D4 mirror it
  uint8_t mode;        // 0x3D8 latch (write-only on real hardware)
  uint8_t color;       // 0x3D9 latch (write-only on real hardware)
  uint8_t status;      // 0x3DA: computed retrace bits + light-pen latch bits
  int dirty;           // guest changed screen state since the last render
  uint32_t version;    // fb content version, bumped by each render
  uint32_t fb[kCgaFbWidth * kCgaFbHeight];
  // render bookkeeping
  uint32_t rendered_blink_tick;  // blink tick the fb was last drawn with
  uint64_t last_render_us;       // render rate gate (host clock)
} CgaDevice;

// 8x8 character generator ROM (device/video/cga_font.c).
extern const uint8_t kCgaFont[256 * 8];

void CgaInit(CgaDevice* d);
// Maps VRAM on the memory bus (on top of RAM: the bus resolves the smaller
// window first) and the card ports at 0x3D0-0x3DF on the I/O bus.
void CgaRegister(Bus* bus, Bus* io, CgaDevice* d);
// Advances retrace status from the host clock and re-renders when the guest
// touched the screen state or a blink phase flipped. Board poll hook, like
// PitPoll.
void CgaPoll(CgaDevice* d);

extern const DeviceOps kCgaPortOps;
extern const DisplaySourceOps kCgaDisplayOps;

#endif
