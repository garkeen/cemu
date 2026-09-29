#include "device/video/vga.h"

#include <string.h>

#include "debug/debug.h"
#include "host/host.h"

// IBM VGA family core (the register map and window layout are in vga.h). The
// register semantics, the aperture decode and the graphics controller's write
// pipeline follow QEMU hw/display/vga.c — the model the local references
// dearchap-tinyemu/vga.c and tiny386/vga.c carry as well — and the power-on
// register contents are the VGA BIOS's standard text-mode tables (SeaVGABIOS
// stdvgamodes.c, "stdvga mode 2": 80x25 text, 8x16 cells, odd/even addressing
// in the B8000 window), so the card comes up exactly where a PC's display is
// before any mode set.

// --- register indices ---------------------------------------------------------
// (IBM VGA Technical Reference; FreeVGA's register reference lists the same
// table. The index latch is 5 bits wide in the CRTC and attribute controller,
// 3 in the sequencer and 4 in the graphics controller.)
enum {
  kCrtcHorizontalTotal = 0x00,
  kCrtcHorizontalDisplayEnd = 0x01,
  kCrtcStartHorizontalBlanking = 0x02,
  kCrtcEndHorizontalBlanking = 0x03,
  kCrtcStartHorizontalRetrace = 0x04,
  kCrtcEndHorizontalRetrace = 0x05,
  kCrtcVerticalTotal = 0x06,
  kCrtcOverflow = 0x07,
  kCrtcPresetRowScan = 0x08,
  kCrtcMaxScanLine = 0x09,
  kCrtcCursorStart = 0x0a,
  kCrtcCursorEnd = 0x0b,
  kCrtcStartAddressHigh = 0x0c,
  kCrtcStartAddressLow = 0x0d,
  kCrtcCursorLocationHigh = 0x0e,
  kCrtcCursorLocationLow = 0x0f,
  kCrtcVerticalRetraceStart = 0x10,
  kCrtcVerticalRetraceEnd = 0x11,
  kCrtcVerticalDisplayEnd = 0x12,
  kCrtcOffset = 0x13,
  kCrtcUnderlineLocation = 0x14,
  kCrtcStartVerticalBlanking = 0x15,
  kCrtcEndVerticalBlanking = 0x16,
  kCrtcModeControl = 0x17,
  kCrtcLineCompare = 0x18,
  kCrtcCount = 0x19,  // R0-R24; the chip has no more registers
};

// CRTC mode control (R17) bits: the low two bits carry the CGA's row and
// vertical-scan address bits, bit 5 wraps the address at 64KB and bit 6
// selects byte (not word) addressing.
enum {
  kCrtcModeCgaCompat = 1u << 0,
  kCrtcModeCgaCompat80 = 1u << 1,
  kCrtcModeAddressWrap = 1u << 5,
  kCrtcModeByteMode = 1u << 6,
};

enum {
  kSeqReset = 0x00,
  kSeqClockMode = 0x01,           // bit 0: 8-dot character clocks (0 = 9-dot)
  kSeqPlaneWriteMask = 0x02,      // which planes a write touches
  kSeqCharacterMapSelect = 0x03,  // font A/B plane offsets (bits 2-5)
  kSeqMemoryMode = 0x04,          // bit 2 odd/even, bit 3 chain 4, bit 1 ext.
};

// Sequencer memory mode (SR4) bits.
enum {
  kSeqMemoryExtended = 1u << 1,
  kSeqMemoryOddEven = 1u << 2,
  kSeqMemoryChain4 = 1u << 3,
};

enum {
  kGfxSetReset = 0x00,
  kGfxEnableSetReset = 0x01,
  kGfxColorCompare = 0x02,
  kGfxDataRotate = 0x03,  // bits 2-0 rotate count, bits 4-3 logical function
  kGfxReadMapSelect = 0x04,
  kGfxMode = 0x05,  // bits 1-0 write mode, bit 3 read mode, bit 4 odd/even
  kGfxMisc = 0x06,  // bits 3-2 memory map mode
  kGfxColorDontCare = 0x07,
  kGfxBitMask = 0x08,
};

// Graphics controller mode (GR5) bits.
enum {
  kGfxModeWriteMask = 3u << 0,
  kGfxModeRead = 1u << 3,
  kGfxModeOddEven = 1u << 4,
  kGfxModeShift = 3u << 5,  // bits 6-5: the picture's pixel-packing mode
};

// Attribute controller registers (AR0-AR15 are the palette entries).
enum {
  kAttrModeControl = 0x10,
  kAttrOverscanColor = 0x11,
  kAttrColorPlaneEnable = 0x12,
  kAttrHorizontalPanning = 0x13,
  kAttrColorSelect = 0x14,
  kAttrCount = 0x15,
};

// Attribute controller mode control (AR10) bits.
enum {
  kAttrModeGraphics = 1u << 0,
  kAttrModeMono = 1u << 1,
  kAttrModeLineGraphics = 1u << 2,
  kAttrModeBlink = 1u << 3,          // attribute bit 7 blinks instead of bright
  kAttrModeLineGraphics9 = 1u << 4,  // 9th dot repeats bit 7 on 0xb0-0xdf
  kAttrModePaletteBits = 1u << 7,    // palette entries carry 6 bits (VGA)
};

// Miscellaneous output register bits (0x3C2 write / 0x3CC read).
enum {
  kMiscColorEmulation = 1u << 0,  // the 0x3D0 half of the port block answers
  kMiscEnableRam = 1u << 1,
  kMiscClockMask = 3u << 2,
  kMiscVsyncPolarity = 1u << 6,
  kMiscHsyncPolarity = 1u << 7,
};

// CGA mode latch (0x3D8) bits (IBM CGA Technical Reference; the VGA keeps
// these latches for the CGA compatibility modes 4/5/6).
enum {
  kCgaMode40Col = 1u << 0,
  kCgaModeGraphics = 1u << 1,
  kCgaModeVideoEnable = 1u << 3,
  kCgaModeBlink = 1u << 5,
};

// Input status bits: status 0 (0x3C2) and status 1 (0x3DA) both report the
// raster; status 1 is the one a colour card's programs poll.
enum {
  kStatusDisplayEnable = 1u << 0,
  kStatusVsync = 1u << 3,
};

// Feature control register: bit 4 is the 0x3DA write path, bit 0 the 0x3CA
// read path (IBM VGA).
enum { kFeatureBit4 = 1u << 4, kFeatureBit0 = 1u << 0 };

// The aperture: 0xA0000-0xBFFFF is one decode. Which part of it answers is the
// graphics controller's memory map mode (GR6 bits 3-2), applied inside the
// memory handlers exactly as QEMU's vga_mem_read/vga_mem_write do.
enum {
  kApertureBase = 0xA0000,
  kApertureSize = 0x20000,
  kApertureMask = 0x1ffff,
  kMapModeDirect = 0,  // A0000-BFFFF: the whole 128KB
  kMapModeA0000 = 1,   // A0000-AFFFF (banked on SVGA; no banking here)
  kMapModeB0000 = 2,   // B0000-B7FFF
  kMapModeB8000 = 3,   // B8000-BFFFF
};

// The text window's base inside the aperture: the odd/even layout puts a
// cell's character in plane 0 and its attribute in plane 1, so the B8000
// window's first cell lives at interleaved byte 0x18000 (QEMU's ch_addr1).
enum { kColorTextBase = 0x18000 };

// The picture the register file currently describes (filled by
// VgaComputePicture below; VgaInit needs the size before the first render, so
// the type and the computation are declared here).
typedef struct VgaPicture {
  int graphics;
  int width, height;  // in pixels
  int cols, rows;     // text grid
  int cell_width;     // text: 8 or 9 dots per cell (SR1 bit 0)
  int cell_height;    // text: scan lines per cell (CRTC R9)
  int odd_even;
  int chain4;
  int shift_control;  // graphics controller GR5 bits 6-5
  int multi_scan;     // extra scan lines each VRAM row is displayed on
  int line_bytes;     // plane offsets per scan line
  int start;          // first displayed plane offset (CRTC R12/R13)
  int planes;         // which planes the picture reads (AR12)
} VgaPicture;

static void VgaComputePicture(const VgaDevice* d, VgaPicture* p);

// --- reset state --------------------------------------------------------------
// Power-on values: the VGA BIOS's standard text-mode tables (SeaVGABIOS
// stdvgamodes.c) — the same numbers QEMU resets its VGA to: the CRTC is
// programmed for 80x25 with 8x16 cells, the sequencer for odd/even plane
// writes, the graphics controller for the B8000 text window, and the DAC holds
// the 64-entry default palette.
static const uint8_t kResetCrtc[kCrtcCount] = {
    0x5f, 0x4f, 0x50, 0x82, 0x55, 0x81, 0xbf, 0x1f, 0x00, 0x4f, 0x0d, 0x0e, 0x00,
    0x00, 0x00, 0x00, 0x9c, 0x8e, 0x8f, 0x28, 0x1f, 0x96, 0xb9, 0xa3, 0xff};
static const uint8_t kResetSequencer[4] = {0x00, 0x03, 0x00, 0x02};
static const uint8_t kResetGraphics[9] = {0x00, 0x00, 0x00, 0x00, 0x00,
                                          0x10, 0x0e, 0x0f, 0xff};
static const uint8_t kResetAttribute[20] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39,
    0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x0c, 0x00, 0x0f, 0x08};
static const uint8_t kResetPalette[64 * 3] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x2a, 0x00, 0x00, 0x2a, 0x2a, 0x2a, 0x00, 0x00, 0x2a,
    0x00, 0x2a, 0x2a, 0x2a, 0x00, 0x2a, 0x2a, 0x2a, 0x00, 0x00, 0x15, 0x00, 0x00, 0x3f, 0x00, 0x2a,
    0x15, 0x00, 0x2a, 0x3f, 0x2a, 0x00, 0x15, 0x2a, 0x00, 0x3f, 0x2a, 0x2a, 0x15, 0x2a, 0x2a, 0x3f,
    0x00, 0x15, 0x00, 0x00, 0x15, 0x2a, 0x00, 0x3f, 0x00, 0x00, 0x3f, 0x2a, 0x2a, 0x15, 0x00, 0x2a,
    0x15, 0x2a, 0x2a, 0x3f, 0x00, 0x2a, 0x3f, 0x2a, 0x00, 0x15, 0x15, 0x00, 0x15, 0x3f, 0x00, 0x3f,
    0x15, 0x00, 0x3f, 0x3f, 0x2a, 0x15, 0x15, 0x2a, 0x15, 0x3f, 0x2a, 0x3f, 0x15, 0x2a, 0x3f, 0x3f,
    0x15, 0x00, 0x00, 0x15, 0x00, 0x2a, 0x15, 0x2a, 0x00, 0x15, 0x2a, 0x2a, 0x3f, 0x00, 0x00, 0x3f,
    0x00, 0x2a, 0x3f, 0x2a, 0x00, 0x3f, 0x2a, 0x2a, 0x15, 0x00, 0x15, 0x15, 0x00, 0x3f, 0x15, 0x2a,
    0x15, 0x15, 0x2a, 0x3f, 0x3f, 0x00, 0x15, 0x3f, 0x00, 0x3f, 0x3f, 0x2a, 0x15, 0x3f, 0x2a, 0x3f,
    0x15, 0x15, 0x00, 0x15, 0x15, 0x2a, 0x15, 0x3f, 0x00, 0x15, 0x3f, 0x2a, 0x3f, 0x15, 0x00, 0x3f,
    0x15, 0x2a, 0x3f, 0x3f, 0x00, 0x3f, 0x3f, 0x2a, 0x15, 0x15, 0x15, 0x15, 0x15, 0x3f, 0x15, 0x3f,
    0x15, 0x15, 0x3f, 0x3f, 0x3f, 0x15, 0x15, 0x3f, 0x15, 0x3f, 0x3f, 0x3f, 0x15, 0x3f, 0x3f, 0x3f};
static const uint8_t kResetMisc = 0x67;
// The CGA latches come up at the CGA's mode-3 values (IBM CGA reference: 80x25
// alphanumeric, video enabled, blink on; black border, palette bit set).
static const uint8_t kResetCgaMode = 0x28;
static const uint8_t kResetCgaColor = 0x30;

void VgaInit(VgaDevice* d) {
  memset(d, 0, sizeof(*d));
  memcpy(d->cr, kResetCrtc, sizeof(kResetCrtc));
  d->sr[kSeqReset] = 0x03;  // the reset register: both sequencer halves run
  memcpy(d->sr + 1, kResetSequencer, sizeof(kResetSequencer));  // SR1-SR4
  memcpy(d->gr, kResetGraphics, sizeof(kResetGraphics));
  memcpy(d->ar, kResetAttribute, sizeof(kResetAttribute));
  d->misc = kResetMisc;
  memcpy(d->dac, kResetPalette, sizeof(kResetPalette));
  d->dac_mask = 0xff;
  d->cga_mode = kResetCgaMode;
  d->cga_color = kResetCgaColor;
  // The reset picture's size, so the display channel has one before the first
  // render (main.c opens the window before the guest runs).
  VgaPicture p;
  VgaComputePicture(d, &p);
  d->fb_width = p.width < kVgaFbWidth ? p.width : kVgaFbWidth;
  d->fb_height = p.height < kVgaFbHeight ? p.height : kVgaFbHeight;
  d->version = 1;
  d->dirty = 1;  // the first poll renders frame 1
}

void VgaRegister(Bus* bus, Bus* io, VgaDevice* d) {
  // One aperture region: A0000-BFFFF is a single decode whose resolution into
  // the planes depends on the register file. It sits inside the board RAM,
  // which the bus resolves last.
  BusAddRegion(bus, kApertureBase, kApertureSize, &kVgaMemOps, d);
  // One port block: 0x3B0-0x3DF. On a VGA the 0x3B0 and 0x3D0 halves are the
  // same registers seen through the monochrome and colour decodes, and the
  // miscellaneous output register's colour bit decides which one answers
  // (QEMU vga_ioport_read/write guard the two ranges with the same test).
  BusAddRegion(io, 0x3b0, 0x30, &kVgaPortOps, d);
}

// --- raster status ------------------------------------------------------------
// The CRTC's own numbers drive the status bits (IBM VGA CRTC register
// definitions: horizontal total = R0 + 5 character clocks, vertical total =
// R6 + 2 scan lines with R7's bits 0/5 as bits 8/9, vertical retrace start =
// R16 with R7's bits 2/7 as bits 8/9, retrace end = R17 bits 3-0, display end =
// R18 with R7's bits 1/6 as bits 8/9). One character clock is 8 or 9 dots of
// the 25.175MHz dot clock (SR1 bit 0), so the frame period is derived from the
// mode the guest programmed — the raster the guest polls, not a fixed model.
enum {
  kDotClockKhz = 25175,  // the VGA's 25.175MHz pixel clock
};

static int VgaVerticalTotal(const VgaDevice* d) {
  return d->cr[kCrtcVerticalTotal] | ((d->cr[kCrtcOverflow] & 0x01) << 8) |
         ((d->cr[kCrtcOverflow] & 0x20) << 4);
}

static int VgaVerticalDisplayEnd(const VgaDevice* d) {
  return d->cr[kCrtcVerticalDisplayEnd] | ((d->cr[kCrtcOverflow] & 0x02) << 7) |
         ((d->cr[kCrtcOverflow] & 0x40) << 3);
}

static int VgaVerticalRetraceStart(const VgaDevice* d) {
  return d->cr[kCrtcVerticalRetraceStart] | ((d->cr[kCrtcOverflow] & 0x04) << 6) |
         ((d->cr[kCrtcOverflow] & 0x80) << 2);
}

static int VgaCharDots(const VgaDevice* d) { return (d->sr[kSeqClockMode] & 1) ? 8 : 9; }

// Latches the raster bits both status ports report. Bit 0 of both is the
// blanking flag (1 while the raster is outside the visible area, which is the
// polarity the CGA reference gives it) and bit 3 is vertical retrace.
static void VgaUpdateStatus(VgaDevice* d, uint64_t now) {
  const int dots = VgaCharDots(d);
  const int line_dots = (d->cr[kCrtcHorizontalTotal] + 5) * dots;
  const int total_lines = VgaVerticalTotal(d) + 2;
  const int visible_lines = VgaVerticalDisplayEnd(d) + 1;
  const int visible_dots = (d->cr[kCrtcHorizontalDisplayEnd] + 1) * dots;
  const int frame_dots = line_dots * total_lines;
  // Frame period in microseconds at the dot clock above, then the position
  // inside it: the same dot count back, so the two steps cannot disagree.
  const int frame_us = (int)((int64_t)frame_dots * 1000 / kDotClockKhz);
  uint64_t t = frame_us > 0 ? (uint64_t)(now % (uint64_t)frame_us) : 0;
  const int pos = (int)(t * kDotClockKhz / 1000);  // dots into the frame
  const int line = pos / line_dots;
  const int dot = pos % line_dots;
  const int retrace_start = VgaVerticalRetraceStart(d);
  const int retrace_end = d->cr[kCrtcVerticalRetraceEnd] & 0x0f;
  int vsync = line >= retrace_start || line <= retrace_end;
  int blanking = !(line < visible_lines && dot < visible_dots);
  uint8_t st = (uint8_t)((vsync ? kStatusVsync : 0) | (blanking ? kStatusDisplayEnable : 0));
  d->st01 = st;
  d->st00 = (uint8_t)(st | (d->feature & kFeatureBit0 ? 0x10 : 0));
}

// --- colour -------------------------------------------------------------------
// The DAC holds 6-bit RGB; the VGA's DAC repeats the top bit in the two low
// bits of the 8-bit result (QEMU's c6_to_8).
static uint32_t DacColor(const VgaDevice* d, int index) {
  const uint8_t* c = &d->dac[(index & 0xff) * 3];
  int v[3];
  for (int i = 0; i < 3; i++) {
    int b = c[i] & 0x3f;
    v[i] = (b << 2) | ((b & 1) << 1) | (b & 1);
  }
  return ((uint32_t)v[0] << 16) | ((uint32_t)v[1] << 8) | (uint32_t)v[2];
}

// A picture colour is an attribute-palette index: AR0-AR15 name the DAC entry,
// with AR10 bit 7 making the entries 6 bits wide (VGA) and the colour-select
// register AR14 supplying the high bits otherwise (IBM VGA; the same arithmetic
// as QEMU's update_palette16).
static int AttrColorIndex(const VgaDevice* d, int index) {
  int v = d->ar[index & 0x0f];
  if (d->ar[kAttrModeControl] & kAttrModePaletteBits)
    return ((d->ar[kAttrColorSelect] & 0x0f) << 4) | (v & 0x0f);
  return ((d->ar[kAttrColorSelect] & 0x0c) << 4) | (v & 0x3f);
}

static uint32_t AttrColor(const VgaDevice* d, int index) {
  return DacColor(d, AttrColorIndex(d, index));
}

// --- ports --------------------------------------------------------------------
// The register file behind its index latches (IBM VGA; QEMU's
// vga_ioport_read/vga_ioport_write is the behavioural cross-check, including
// the write mask each register applies and the CRTC's R0-R7 protection).
enum {
  kPortCrtcMonoIndex = 0x3b4,
  kPortCrtcMonoData = 0x3b5,
  kPortCrtcMonoStatus = 0x3ba,
  kPortAttribute = 0x3c0,
  kPortAttributeRead = 0x3c1,
  kPortMiscWrite = 0x3c2,
  kPortStatus0 = 0x3c2,
  kPortSeqIndex = 0x3c4,
  kPortSeqData = 0x3c5,
  kPortDacMask = 0x3c6,
  kPortDacReadIndex = 0x3c7,
  kPortDacWriteIndex = 0x3c8,
  kPortDacData = 0x3c9,
  kPortFeatureRead = 0x3ca,
  kPortMiscRead = 0x3cc,
  kPortGfxIndex = 0x3ce,
  kPortGfxData = 0x3cf,
  kPortCrtcIndex = 0x3d4,
  kPortCrtcData = 0x3d5,
  kPortCgaMode = 0x3d8,
  kPortCgaColor = 0x3d9,
  kPortStatus1 = 0x3da,
};

// Reserved bits read back as zero: each register keeps only the bits the chip
// implements (QEMU's sr_mask/gr_mask tables; the attribute controller masks
// its own in the write path).
static const uint8_t kSeqWriteMask[8] = {0x03, 0x3b, 0x0f, 0x1f, 0x0f, 0x00, 0x00, 0x00};
static const uint8_t kGfxWriteMask[16] = {0x0f, 0x0f, 0x0f, 0x1f, 0x03, 0x7f, 0x0f,
                                          0x0f, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0x00, 0x00};

// The colour and monochrome halves of the port block are mutually exclusive:
// only the half the miscellaneous output register selects answers, and the
// other floats high (QEMU guards the two ranges with this same test).
static int VgaPortDecoded(const VgaDevice* d, uint16_t port) {
  if (port >= 0x3b0 && port <= 0x3bf) return (d->misc & kMiscColorEmulation) ? 0 : 1;
  if (port >= 0x3d0 && port <= 0x3df) return (d->misc & kMiscColorEmulation) ? 1 : 0;
  return 1;
}

static uint8_t VgaPortRead8(VgaDevice* d, uint16_t port) {
  if (!VgaPortDecoded(d, port)) return 0xff;
  switch (port) {
    case kPortAttribute:  // the index, while the flip-flop is clear
      return d->ar_flip_flop == 0 ? d->ar_index : 0;
    case kPortAttributeRead: {
      int i = d->ar_index & 0x1f;
      return i < kAttrCount ? d->ar[i] : 0;
    }
    case kPortStatus0:
      VgaUpdateStatus(d, (uint64_t)HostTimerNow());
      return d->st00;
    case kPortSeqIndex:
      return d->sr_index;
    case kPortSeqData:
      return d->sr[d->sr_index];
    case kPortDacMask:
      return d->dac_mask;
    case kPortDacReadIndex:
      return d->dac_state;
    case kPortDacWriteIndex:
      return d->dac_write_index;
    case kPortDacData: {
      uint8_t v = d->dac[d->dac_read_index * 3 + d->dac_sub_index];
      if (++d->dac_sub_index == 3) {
        d->dac_sub_index = 0;
        d->dac_read_index++;
      }
      return v;
    }
    case kPortFeatureRead:
      return d->feature;
    case kPortMiscRead:
      return d->misc;
    case kPortGfxIndex:
      return d->gr_index;
    case kPortGfxData:
      return d->gr[d->gr_index];
    case kPortCrtcMonoIndex:
    case kPortCrtcIndex:
      return d->cr_index;  // the index is readable; probes and guests use it
    case kPortCrtcMonoData:
    case kPortCrtcData:
      return d->cr_index < kCrtcCount ? d->cr[d->cr_index] : 0;
    case kPortCrtcMonoStatus:
    case kPortStatus1:
      VgaUpdateStatus(d, (uint64_t)HostTimerNow());
      d->ar_flip_flop = 0;  // reading status 1 resets the attribute flip-flop
      return d->st01;
    default:
      return 0xff;  // the block's undefined ports float high
  }
}

static void VgaPortWrite8(VgaDevice* d, uint16_t port, uint8_t val) {
  if (!VgaPortDecoded(d, port)) return;
  switch (port) {
    case kPortAttribute:  // index while the flip-flop is clear, data while set
      if (d->ar_flip_flop == 0) {
        d->ar_index = val & 0x3f;
      } else {
        int i = d->ar_index & 0x1f;
        if (i <= 0x0f)
          d->ar[i] = val & 0x3f;  // palette entries carry 6 bits
        else if (i == kAttrModeControl)
          d->ar[i] = val & ~0x10;  // bit 4 is reserved on a VGA
        else if (i == kAttrOverscanColor)
          d->ar[i] = val;
        else if (i == kAttrColorPlaneEnable)
          d->ar[i] = val & ~0xc0;
        else if (i == kAttrHorizontalPanning || i == kAttrColorSelect)
          d->ar[i] = val & ~0xf0;
        // AR21-AR31 do not exist.
      }
      d->ar_flip_flop ^= 1;
      break;
    case kPortMiscWrite:
      d->misc = val & ~0x10;  // bit 4 is not stored (QEMU vga_ioport_write)
      break;
    case kPortSeqIndex:
      d->sr_index = val & 7;
      break;
    case kPortSeqData:
      d->sr[d->sr_index] = val & kSeqWriteMask[d->sr_index];
      break;
    case kPortDacMask:
      d->dac_mask = val;  // affects CPU reads of the DAC, not the picture
      return;
    case kPortDacReadIndex:  // starting a read takes three bytes per entry
      d->dac_read_index = val;
      d->dac_sub_index = 0;
      d->dac_state = 3;
      return;
    case kPortDacWriteIndex:
      d->dac_write_index = val;
      d->dac_sub_index = 0;
      d->dac_state = 0;
      return;
    case kPortDacData:
      d->dac_cache[d->dac_sub_index] = val;
      if (++d->dac_sub_index != 3) return;  // the colour lands on the third byte
      memcpy(&d->dac[d->dac_write_index * 3], d->dac_cache, 3);
      d->dac_sub_index = 0;
      d->dac_write_index++;
      break;
    case kPortGfxIndex:
      d->gr_index = val & 0x0f;
      return;
    case kPortGfxData:
      d->gr[d->gr_index] = val & kGfxWriteMask[d->gr_index];
      break;
    case kPortCrtcMonoIndex:
    case kPortCrtcIndex:
      d->cr_index = val & 0x1f;  // the CRTC's index latch is 5 bits
      return;
    case kPortCrtcMonoData:
    case kPortCrtcData:
      // CR11 bit 7 protects R0-R7; only bit 4 of R7 stays writable (IBM VGA;
      // the same guard as QEMU's vga_ioport_write).
      if ((d->cr[kCrtcVerticalRetraceEnd] & 0x80) && d->cr_index <= 7) {
        if (d->cr_index == kCrtcOverflow)
          d->cr[kCrtcOverflow] = (uint8_t)((d->cr[kCrtcOverflow] & ~0x10) | (val & 0x10));
        return;
      }
      if (d->cr_index < kCrtcCount) d->cr[d->cr_index] = val;
      break;
    case kPortCgaMode:
      d->cga_mode = val;  // the CGA compatibility latches (modes 4/5/6)
      break;
    case kPortCgaColor:
      d->cga_color = val;
      break;
    case kPortCrtcMonoStatus:
    case kPortStatus1:
      d->feature = val & kFeatureBit4;  // the write path is feature control
      return;
    default:
      return;
  }
  d->dirty = 1;  // a register that could change the picture changed
}

// The card's ports are byte-wide; a 16- or 32-bit cycle drives the byte lanes
// in turn (QEMU's VGA portio does the same), because that is how ISA/VLB
// software programs the index/data pairs (`outw` to 0x3C4 sets index and data).
static uint64_t VgaPortRead(void* dev, uint64_t addr, int size) {
  VgaDevice* d = (VgaDevice*)dev;
  uint64_t v = 0;
  for (int i = 0; i < size; i++)
    v |= (uint64_t)VgaPortRead8(d, (uint16_t)(addr + (uint64_t)i)) << (8 * i);
  return v;
}

static void VgaPortWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  VgaDevice* d = (VgaDevice*)dev;
  for (int i = 0; i < size; i++)
    VgaPortWrite8(d, (uint16_t)(addr + (uint64_t)i), (uint8_t)(val >> (8 * i)));
}

const DeviceOps kVgaPortOps = {"vga", VgaPortRead, VgaPortWrite};

// --- video memory --------------------------------------------------------------
// The aperture resolves into the planes the way QEMU's vga_mem_write and
// vga_mem_read do: the graphics controller's memory mode (GR6 bits 3-2) picks
// the window, the sequencer's chain-4 bit and the graphics controller's
// odd/even bit pick the plane(s) an access touches, and a write's data runs
// through the rotate, set/reset, bit-mask and logical-operation stages of write
// modes 0-3. A read latches all four planes first and answers per read mode
// 0/1 (IBM VGA; QEMU's model).
static const uint32_t kPlaneExpand[16] = {
    0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff, 0x00ff0000, 0x00ff00ff,
    0x00ffff00, 0x00ffffff, 0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff,
    0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
};

// The offset inside the planes a guest address means, or -1 when the address is
// outside the window this memory mode maps.
static int VgaApertureOffset(const VgaDevice* d, uint32_t addr) {
  addr &= kApertureMask;
  switch ((d->gr[kGfxMisc] >> 2) & 3) {
    case kMapModeA0000:
      if (addr >= 0x10000) return -1;
      break;
    case kMapModeB0000:
      addr -= 0x10000;
      if (addr >= 0x8000) return -1;
      break;
    case kMapModeB8000:
      addr -= 0x18000;
      if (addr >= 0x8000) return -1;
      break;
    default:  // A0000-BFFFF maps the planes directly
      break;
  }
  return (int)addr;
}

static int VgaOddEvenOffset(const VgaDevice* d, int off) {
  int plane = (d->gr[kGfxReadMapSelect] & 2) | (off & 1);
  return (int)(((uint32_t)off & ~1u) << 1 | (uint32_t)plane);
}

static void VgaMemWrite8(VgaDevice* d, uint32_t addr, uint8_t val) {
  int off = VgaApertureOffset(d, addr);
  if (off < 0) return;
  if (d->sr[kSeqMemoryMode] & kSeqMemoryChain4) {
    // Chain 4: the address itself selects the plane, one byte per plane byte.
    int plane = off & 3;
    if (d->sr[kSeqPlaneWriteMask] & (1u << plane)) d->vram[off] = val;
    return;
  }
  if (d->gr[kGfxMode] & kGfxModeOddEven) {
    // Odd/even (the text layout): even addresses are plane 0, odd ones are
    // plane 1, and the offset doubles.
    int target = VgaOddEvenOffset(d, off);
    int plane = target & 3;
    if (d->sr[kSeqPlaneWriteMask] & (1u << plane)) d->vram[target] = val;
    return;
  }
  uint32_t data;
  uint32_t bit_mask;
  int rot = d->gr[kGfxDataRotate] & 7;
  switch (d->gr[kGfxMode] & kGfxModeWriteMask) {
    case 1:  // write mode 1: the latched planes go straight back
      data = d->latch;
      bit_mask = 0xffffffff;
      goto write_planes;
    case 2:  // write mode 2: the value's low nibble expands across the planes
      data = kPlaneExpand[val & 0x0f];
      bit_mask = kPlaneExpand[d->gr[kGfxBitMask] & 0x0f];
      break;
    case 3: {  // write mode 3: set/reset data, bit mask from the value
      uint32_t v = (uint32_t)(((val >> rot) | (val << (8 - rot))) & 0xff);
      bit_mask = (uint32_t)(d->gr[kGfxBitMask] & v);
      data = kPlaneExpand[d->gr[kGfxSetReset]];
      break;
    }
    default: {  // write mode 0: rotate, set/reset, then the bit mask
      uint32_t v = (uint32_t)(((val >> rot) | (val << (8 - rot))) & 0xff);
      v |= v << 8;
      v |= v << 16;
      uint32_t set_mask = kPlaneExpand[d->gr[kGfxEnableSetReset] & 0x0f];
      data = (v & ~set_mask) | (kPlaneExpand[d->gr[kGfxSetReset]] & set_mask);
      bit_mask = kPlaneExpand[d->gr[kGfxBitMask] & 0x0f];
      break;
    }
  }
  // The logical operation (GR3 bits 4-3) combines the data with the latched
  // planes, then the bit mask says which bits may change at all.
  switch ((d->gr[kGfxDataRotate] >> 3) & 3) {
    case 1:
      data &= d->latch;
      break;
    case 2:
      data |= d->latch;
      break;
    case 3:
      data ^= d->latch;
      break;
    default:
      break;
  }
  bit_mask |= bit_mask << 8;
  bit_mask |= bit_mask << 16;
  data = (data & bit_mask) | (d->latch & ~bit_mask);
write_planes:;
  uint32_t write_mask = kPlaneExpand[d->sr[kSeqPlaneWriteMask] & 0x0f];
  uint32_t* cell = &((uint32_t*)d->vram)[off];
  *cell = (*cell & ~write_mask) | (data & write_mask);
}

static uint8_t VgaMemRead8(VgaDevice* d, uint32_t addr) {
  int off = VgaApertureOffset(d, addr);
  if (off < 0) return 0xff;
  if (d->sr[kSeqMemoryMode] & kSeqMemoryChain4) return d->vram[off];
  if (d->gr[kGfxMode] & kGfxModeOddEven) return d->vram[VgaOddEvenOffset(d, off)];
  d->latch = ((const uint32_t*)d->vram)[off];  // every read latches all planes
  if (d->gr[kGfxMode] & kGfxModeRead) {
    // Read mode 1: compare the latched planes with the colour-compare value
    // under the colour-don't-care mask and report a 1 where they differ.
    uint32_t ret = (d->latch ^ kPlaneExpand[d->gr[kGfxColorCompare] & 0x0f]) &
                   kPlaneExpand[d->gr[kGfxColorDontCare] & 0x0f];
    ret |= ret >> 16;
    ret |= ret >> 8;
    return (uint8_t)~ret;
  }
  // Read mode 0: the plane the read-map-select register names.
  return (uint8_t)(d->latch >> (8 * (d->gr[kGfxReadMapSelect] & 3)));
}

static uint64_t VgaMemRead(void* dev, uint64_t addr, int size) {
  VgaDevice* d = (VgaDevice*)dev;
  uint32_t off = (uint32_t)(addr - kApertureBase);
  uint64_t v = 0;
  for (int i = 0; i < size; i++)
    v |= (uint64_t)VgaMemRead8(d, off + (uint32_t)i) << (8 * i);
  return v;
}

static void VgaMemWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  VgaDevice* d = (VgaDevice*)dev;
  uint32_t off = (uint32_t)(addr - kApertureBase);
  for (int i = 0; i < size; i++)
    VgaMemWrite8(d, off + (uint32_t)i, (uint8_t)(val >> (8 * i)));
  d->dirty = 1;
}

const DeviceOps kVgaMemOps = {"vga-vram", VgaMemRead, VgaMemWrite};

// --- rendering -------------------------------------------------------------------
// The picture is derived from the register file every time it changes, the way
// the hardware's address generator derives it: the CRTC's timing registers give
// the geometry, the sequencer's clock mode and the graphics controller's shift
// bits give the pixel packing, and the attribute controller maps pixel values to
// DAC entries (IBM VGA). Three pictures are drawable — text, the 16-colour
// planar picture (the EGA/VGA modes, including 640x350) and the CGA modes 4/5/6,
// which are its 2bpp and 1bpp shapes. Chained 256-colour pictures (mode 13h) are
// addressed by the memory path but not drawn; that gap is registered in
// AGENTS.md 简化登记 D17.
static void VgaComputePicture(const VgaDevice* d, VgaPicture* p) {
  memset(p, 0, sizeof(*p));
  const int display_end = VgaVerticalDisplayEnd(d);
  p->graphics = (d->ar[kAttrModeControl] & kAttrModeGraphics) != 0;
  p->odd_even = (d->gr[kGfxMode] & kGfxModeOddEven) != 0;
  p->chain4 = (d->sr[kSeqMemoryMode] & kSeqMemoryChain4) != 0;
  p->shift_control = (d->gr[kGfxMode] & kGfxModeShift) >> 5;
  p->planes = d->ar[kAttrColorPlaneEnable] & 0x0f;
  p->cell_width = VgaCharDots(d);
  p->cell_height = (d->cr[kCrtcMaxScanLine] & 0x1f) + 1;
  p->start = (d->cr[kCrtcStartAddressHigh] << 8) | d->cr[kCrtcStartAddressLow];
  // The CRTC's offset register counts words; a word is two plane offsets, which
  // is also one text cell (character + attribute) in the odd/even layout.
  p->line_bytes = d->cr[kCrtcOffset] << 1;
  if (p->graphics) {
    p->width = (d->cr[kCrtcHorizontalDisplayEnd] + 1) * 8;
    p->height = display_end + 1;
    // CRTC R9: bit 7 doubles every row (the 200-to-400 line conversion), the
    // low bits repeat each VRAM row that many extra times.
    p->multi_scan = (d->cr[kCrtcMaxScanLine] & 0x80) ? 1 : (d->cr[kCrtcMaxScanLine] & 0x1f);
  } else {
    p->cols = d->cr[kCrtcHorizontalDisplayEnd] + 1;
    p->rows = (display_end + 1) / p->cell_height;
    p->width = p->cols * p->cell_width;
    p->height = p->rows * p->cell_height;
  }
}

static void VgaRenderText(VgaDevice* d, const VgaPicture* p, uint32_t blink_tick) {
  // The character generator is plane 2, where the BIOS loads it at mode set
  // (SeaVGABIOS stdvgamodes.c stdvga_load_font): SR3 picks the 8KB block font A
  // and font B come from, attribute bit 3 picks the font, a glyph is 32 bytes
  // (one per scan line, bit 7 leftmost) and each byte sits four bytes apart in
  // the interleaved plane layout (QEMU's font_base arithmetic).
  const uint8_t sel = d->sr[kSeqCharacterMapSelect];
  const int font_a = (int)(((sel >> 4) & 1) | ((sel << 1) & 6)) * 8192 * 4 + 2;
  const int font_b = (int)(((sel >> 5) & 1) | ((sel >> 1) & 6)) * 8192 * 4 + 2;
  const int blink = (d->ar[kAttrModeControl] & kAttrModeBlink) != 0;
  const int blink_off = blink && (blink_tick & 1);
  const int cursor_cell =
      ((d->cr[kCrtcCursorLocationHigh] << 8) | d->cr[kCrtcCursorLocationLow]) - p->start;
  const int cursor_shown = !(d->cr[kCrtcCursorStart] & 0x20) && !(blink && ((blink_tick >> 1) & 1));
  const int cursor_first = d->cr[kCrtcCursorStart] & 0x1f;
  const int cursor_last = d->cr[kCrtcCursorEnd] & 0x1f;
  for (int row = 0; row < p->rows; row++) {
    for (int col = 0; col < p->cols; col++) {
      const int off = p->start + row * p->line_bytes + col;
      const int px = col * p->cell_width;
      if (px >= kVgaFbWidth) break;
      // In the odd/even layout a cell is one plane offset: the character in
      // plane 0 and its attribute in plane 1.
      const uint16_t ch_attr = (uint16_t)(d->vram[off * 4] | (d->vram[off * 4 + 1] << 8));
      const uint8_t ch = (uint8_t)(ch_attr & 0xff);
      const uint8_t attr = (uint8_t)(ch_attr >> 8);
      int fg = attr & 0x0f;
      int bg = (attr >> 4) & 0x0f;
      if (blink) bg &= 0x07;  // with blink on, bg bit 3 is the blink flag
      if (blink_off && (attr & 0x80))
        fg = bg;  // the blink phase shows the cell's background
      const int glyph = ((attr & 0x08) ? font_b : font_a) + ch * 128;
      const int dup9 = p->cell_width == 9 && (d->ar[kAttrModeControl] & kAttrModeLineGraphics9) &&
                       ch >= 0xb0 && ch <= 0xdf;
      for (int line = 0; line < p->cell_height; line++) {
        const int y = row * p->cell_height + line;
        if (y >= p->height || y >= kVgaFbHeight) break;
        const uint8_t bits = d->vram[glyph + line * 4];
        uint32_t* out = &d->fb[(size_t)y * kVgaFbWidth + px];
        const uint32_t fg_color = AttrColor(d, fg);
        const uint32_t bg_color = AttrColor(d, bg);
        for (int dot = 0; dot < p->cell_width && px + dot < kVgaFbWidth; dot++) {
          int on = dot < 8 ? (bits >> (7 - dot)) & 1 : (dup9 ? bits & 1 : 0);
          out[dot] = on ? fg_color : bg_color;
        }
        if (cursor_shown && row * p->line_bytes + col == cursor_cell && line >= cursor_first &&
            line <= cursor_last) {
          for (int dot = 0; dot < p->cell_width && px + dot < kVgaFbWidth; dot++)
            out[dot] = fg_color;
        }
      }
    }
  }
}

static void VgaRenderGraphics(VgaDevice* d, const VgaPicture* p) {
  // Two shapes: the planar picture (shift control 0), where a pixel's value is
  // one bit from each enabled plane at the same bit position, and the CGA's
  // packed 2bpp shape (shift control 1, modes 4/5), where a byte holds four
  // pixels and even/odd bytes alternate between plane 0 and plane 1 (IBM CGA
  // Technical Reference; the same packing QEMU's planar loop walks). The CGA
  // shapes also honour the 0x3D8 video-enable latch and the 0x3D9 colour latch,
  // whose background, palette and intensity bits select the attribute-palette
  // entries the CGA colours map to — the values the BIOS's own CGA-mode tables
  // encode, so a guest that leaves the latches alone sees the standard colours.
  const int cga_colors = p->shift_control == 1;
  const int cga_mono = p->planes == 1;  // mode 6: one plane, two colours
  const int cga = cga_colors || cga_mono;
  const int blank = cga && !(d->cga_mode & kCgaModeVideoEnable);
  const int intensity = (d->cga_color & 0x10) ? 8 : 0;
  const int palette_base = (d->cga_color & 0x20) ? 3 : 2;
  for (int y = 0; y < p->height && y < kVgaFbHeight; y++) {
    const int row = y / (p->multi_scan + 1);
    const uint32_t base = (uint32_t)(p->start + row * p->line_bytes);
    uint32_t* out = &d->fb[(size_t)y * kVgaFbWidth];
    for (int x = 0; x < p->width && x < kVgaFbWidth; x++) {
      if (blank) {
        out[x] = 0;
        continue;
      }
      int value;
      if (cga_colors) {
        uint8_t byte = d->vram[(base + (uint32_t)(x >> 3)) * 4 + (uint32_t)((x & 4) >> 2)];
        value = (byte >> (6 - 2 * (x & 3))) & 3;
      } else {
        const uint32_t off = base + (uint32_t)(x >> 3);
        const int bit = 7 - (x & 7);
        int v = 0;
        for (int pl = 0; pl < 4; pl++)
          if (p->planes & (1 << pl)) v |= ((d->vram[off * 4 + (uint32_t)pl] >> bit) & 1) << pl;
        value = v;
      }
      int index;
      if (cga && value == 0)
        index = d->cga_color & 0x0f;  // the CGA background/border colour
      else if (cga_colors)
        index = intensity | (value == 1 ? palette_base : value == 2 ? palette_base + 2
                                                                     : palette_base + 4);
      else if (cga_mono)
        index = (d->cga_color >> 4) & 0x0f;  // 640x200: bits 4-7 are the ink
      else
        index = value;  // the 16-colour planar picture: AR0-AR15
      out[x] = AttrColor(d, index);
    }
  }
}

// --- observation and refresh ------------------------------------------------------
// The guest prints by writing the text plane, so a guest whose console never
// touches the serial port can only be read back through the screen mirror
// (CEMU_DEBUG=screen, AGENTS.md §X). It diffs the character plane of the current
// text picture each render and emits the rows that changed.
static void ScreenMirror(VgaDevice* d, const VgaPicture* p) {
  if (!DebugOn(kDbgScreen) || p->graphics) return;
  static int once;
  if (!once) {
    once = 1;
    DebugText("screen-run", "mirror reached");
  }
  const int cols = p->cols < kVgaMirrorCols ? p->cols : kVgaMirrorCols;
  const int rows = p->rows < kVgaMirrorRows ? p->rows : kVgaMirrorRows;
  for (int row = 0; row < rows; row++) {
    char line[kVgaMirrorCols + 1];
    for (int col = 0; col < cols; col++) {
      uint8_t ch = d->vram[(p->start + row * p->line_bytes + col) * 4];
      line[col] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : ' ';
    }
    line[cols] = 0;
    int len = cols;
    while (len > 0 && line[len - 1] == ' ') line[--len] = 0;
    if (strcmp(d->mirror[row], line) == 0) continue;
    strcpy(d->mirror[row], line);
    DebugText("screen", line);
  }
}

// Blink periods follow the field rate: the attribute's blink flag flips every
// sixteen fields and the cursor every thirty-two (IBM VGA: both derive from
// vertical sync), which at 60Hz is ~266ms and ~533ms. Host time is the clock
// this model has, so a tick is a wall-clock multiple of the field period — the
// same approximation the CGA slice used (registered in D17).
enum { kBlinkTickUs = 133 * 1000, kRenderIntervalUs = 4000 };

void VgaPoll(VgaDevice* d) {
  const uint64_t now = (uint64_t)HostTimerNow();
  VgaUpdateStatus(d, now);
  const uint32_t blink_tick = (uint32_t)(now / kBlinkTickUs);
  if (!d->dirty && blink_tick == d->rendered_blink_tick) return;
  if (now - d->last_render_us < kRenderIntervalUs) return;  // batch guest writes
  VgaPicture p;
  VgaComputePicture(d, &p);
  d->fb_width = p.width < kVgaFbWidth ? p.width : kVgaFbWidth;
  d->fb_height = p.height < kVgaFbHeight ? p.height : kVgaFbHeight;
  if (p.graphics)
    VgaRenderGraphics(d, &p);
  else
    VgaRenderText(d, &p, blink_tick);
  ScreenMirror(d, &p);
  d->rendered_blink_tick = blink_tick;
  d->last_render_us = now;
  d->dirty = 0;
  d->version++;
}

// --- display channel ---------------------------------------------------------------
static int VgaDisplayWidth(void* dev) { return ((VgaDevice*)dev)->fb_width; }
static int VgaDisplayHeight(void* dev) { return ((VgaDevice*)dev)->fb_height; }
static const uint32_t* VgaDisplayFramebuffer(void* dev) { return ((VgaDevice*)dev)->fb; }
static uint32_t VgaDisplayVersion(void* dev) { return ((VgaDevice*)dev)->version; }

const DisplaySourceOps kVgaDisplayOps = {VgaDisplayWidth, VgaDisplayHeight, VgaDisplayFramebuffer,
                                         VgaDisplayVersion};
