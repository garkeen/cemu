#ifndef CEMU_DEVICE_VIDEO_VGA_H
#define CEMU_DEVICE_VIDEO_VGA_H

#include <stdint.h>

#include "bus/bus.h"
#include "device/video/display.h"

// IBM VGA family core (阶段 4 图形切片). CGA, EGA and VGA are three faces of
// one card family, so they are three compatibility layers of this core rather
// than three separate cards (user ruling, progress.md 片 25): one plane-based
// VRAM, the four register groups at 0x3C0-0x3DF, planar and chained
// addressing, and a 256-entry colour DAC. EGA 640x350 — the mode Windows
// 1.01's display driver asks for (acceptance 4) — is the same register model
// as VGA 640x480, and the CGA modes 4/5/6 are its 2bpp/1bpp planar shapes.
//
// Register map and addressing: IBM VGA Technical Reference (the four groups,
// the memory maps, the odd/even text layout), IBM CGA Technical Reference (the
// 0x3D8/0x3D9 compatibility latches and the CGA colour model), FreeVGA's
// register reference. The behavioural cross-check for the graphics
// controller's write/read modes and the register masking is QEMU
// hw/display/vga.c (the model dearchap-tinyemu and tiny386 carry as well).
//
// Ports:
//   0x3B4/0x3B5  CRTC index/data (monochrome decode)
//   0x3C0/0x3C1  attribute controller index/data (shared flip-flop)
//   0x3C2        miscellaneous output (write) / input status 0 (read)
//   0x3C4/0x3C5  sequencer
//   0x3C6-0x3C9  DAC: PEL mask, read index, write index, data
//   0x3CA        feature control (read)
//   0x3CC        miscellaneous output (read)
//   0x3CE/0x3CF  graphics controller
//   0x3D0-0x3D3  CGA CRTC mirrors; 0x3D4/0x3D5 CRTC (colour decode)
//   0x3D8/0x3D9  CGA mode/colour latches (CGA compatibility)
//   0x3DA        input status 1 (read) / feature control (write)
//
// Memory windows: A0000-AFFFF (graphics) and B0000-B7FFF / B8000-BFFFF (text)
// resolve into the planes through the graphics controller's memory mode; in
// text modes the odd/even scheme keeps each cell's character in plane 0 and
// its attribute in plane 1 — which is also what CEMU_DEBUG's `screen` mirror
// reads back (AGENTS.md §X).
enum {
  kVgaPlaneSize = 0x10000,  // 64KB per plane; four planes = 256KB
  kVgaVramSize = 4 * kVgaPlaneSize,
  // Widest picture this renderer draws. Wider modes are clipped rather than
  // overrunning the buffer (AGENTS.md 简化登记 D17).
  kVgaFbWidth = 800,
  kVgaFbHeight = 600,
  // The text mirror's grid: enough for 132x60 (the largest text mode the CRTC
  // registers can describe here); the mirror prints the current mode's grid.
  kVgaMirrorCols = 132,
  kVgaMirrorRows = 60,
};

typedef struct VgaDevice {
  // The 256KB of video memory as four planes, laid out dword-interleaved the
  // way the graphics controller reaches them: vram[offset * 4 + plane] is
  // plane `plane` at offset. The read latch holds the four plane bytes the
  // last fetch read; read mode 1 and write modes 1-3 act on it.
  uint8_t vram[kVgaVramSize];
  uint32_t latch;
  uint8_t cr[0x19];  // CRTC R0-R24 (the chip has no more registers)
  uint8_t cr_index;
  uint8_t sr[8];  // sequencer SR0-SR7
  uint8_t sr_index;
  uint8_t gr[0x10];  // graphics controller GR0-GR15
  uint8_t gr_index;
  uint8_t ar[0x15];  // attribute controller AR0-AR20
  uint8_t ar_index;
  int ar_flip_flop;
  uint8_t misc;     // miscellaneous output (0x3C2 write / 0x3CC read)
  uint8_t feature;  // feature control (0x3CA read / 0x3DA write)
  uint8_t st00;     // input status 0: latched when 0x3C2 is read
  uint8_t st01;     // input status 1: vertical retrace, display enable
  uint8_t cga_mode;   // 0x3D8 latch: CGA video enable / 40-col / graphics / blink
  uint8_t cga_color;  // 0x3D9 latch: CGA border, palette select, intensity
  // DAC (0x3C6-0x3C9): 256 6-bit RGB entries, the PEL mask, and the read and
  // write index state machines (each data byte is one colour component).
  uint8_t dac[256 * 3];
  uint8_t dac_mask;
  uint8_t dac_read_index;
  uint8_t dac_write_index;
  uint8_t dac_sub_index;
  uint8_t dac_state;
  uint8_t dac_cache[3];
  // Rendering: the guest's register writes mark the picture dirty; Poll
  // re-renders at most once per interval into fb, whose size is the current
  // mode's (fb_width x fb_height) and which the display channel publishes.
  int dirty;
  uint32_t version;
  uint32_t fb[kVgaFbWidth * kVgaFbHeight];
  int fb_width;
  int fb_height;
  uint64_t last_render_us;
  uint32_t rendered_blink_tick;
  // The text grid the `screen` mirror last printed, so only changed rows are
  // emitted.
  char mirror[kVgaMirrorRows][kVgaMirrorCols + 1];
} VgaDevice;

void VgaInit(VgaDevice* d);
// Maps the three memory windows on the memory bus (on top of RAM: the bus
// resolves the smaller window first) and the card's port block at
// 0x3B0-0x3DF (0x3B0-0x3BF and 0x3D0-0x3DF are one decode on a VGA: the
// colour/monochrome bit in the miscellaneous output register decides which
// half answers) on the I/O bus.
void VgaRegister(Bus* bus, Bus* io, VgaDevice* d);
// Advances the raster status from the host clock and re-renders when the guest
// touched the register file or the blink phase flipped. Board poll hook, like
// PitPoll.
void VgaPoll(VgaDevice* d);

extern const DeviceOps kVgaPortOps;
// The aperture's device ops (0xA0000-0xBFFFF): VgaRegister maps it, and the
// board never touches it directly.
extern const DeviceOps kVgaMemOps;
extern const DisplaySourceOps kVgaDisplayOps;

#endif
