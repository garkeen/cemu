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

// Feature control register: bit 4 is the 0x3DA write path (the read path at
// 0x3CA gives it back); the register holds nothing else (IBM VGA).
enum { kFeatureBit4 = 1u << 4 };

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
  // DISPI is a second block of its own: the option ROM's VBE probe must answer
  // whatever half of the register file is currently decoded, so it cannot live
  // inside the 0x3B0-0x3DF range. It is four bytes rather than two because the
  // bus requires a whole access to fit one region, and the ROM's inw at 0x1CF
  // covers 0x1CF-0x1D0.
  BusAddRegion(io, kDispiIndexPort, kDispiPortCount, &kVgaPortOps, d);
}

// --- raster status ------------------------------------------------------------
// The two status ports report where the raster is inside the frame the CRTC is
// programmed to scan, so every number below is read out of the register file
// (IBM VGA Technical Reference, CRTC register set):
//
//   horizontal total      R0 + 5 character clocks, doubled when the sequencer
//                         runs 9-dot character addressing (SR1 bit 3)
//   vertical total        (R6 | R7 bit0 as bit8 | R7 bit5 as bit9) + 2 lines
//   vertical retrace      starts at (R16 | R7 bit2 as bit8 | R7 bit7 as bit9),
//                         lasts R17 bits 3-0 + 1 lines
//   horizontal retrace    starts at R4 plus a skew from R5 bits 6-5, lasts
//                         R5 bits 4-0 + 1 character clocks
//   character clock       8 or 9 dots, selected by miscellaneous output bit 0
//   dot clock             the oscillator miscellaneous output bits 3-2 select
//
// Bit 0 of the status ports is the blanking flag (set while the raster is not
// drawing the visible area -- the polarity the CGA reference gives it), bit 3
// the vertical retrace. The position comes from the emulated clock, which is
// instruction-paced under -icount, so a run stays reproducible while a guest
// that polls the port still watches the flag move across a frame.
enum {
  kNsPerSecond = 1000000000,
  kNsPerMicroSecond = 1000,
  kField2Bits = 0x03,        // the two-bit fields below (clock select, skew)
  kField5Bits = 0x1f,        // R5 bits 4-0: horizontal retrace end
  kField4Bits = 0x0f,        // R17 bits 3-0: vertical retrace length
  kMsrDots8 = 1u << 0,       // 8 dot character clock (clear selects 9)
  kMsrClockSelShift = 2,     // which oscillator the two bits above choose
};

// The four clocks the miscellaneous output register can pick (IBM VGA: only the
// 25.175MHz and 28.322MHz crystals are fitted; the two remaining selections
// repeat one of them).
static const int kVgaDotClockHz[4] = {25175000, 28322000, 25175000, 25175000};

static int VgaVerticalTotal(const VgaDevice* d) {
  return d->cr[kCrtcVerticalTotal] | ((d->cr[kCrtcOverflow] & 0x01) << 8) |
         ((d->cr[kCrtcOverflow] & 0x20) << 4);
}

static int VgaVerticalRetraceStart(const VgaDevice* d) {
  return d->cr[kCrtcVerticalRetraceStart] | ((d->cr[kCrtcOverflow] & 0x04) << 6) |
         ((d->cr[kCrtcOverflow] & 0x80) << 2);
}

// The last line the CRTC draws before blanking: R18 with R7's bits 1 and 6 as
// bits 8 and 9 (IBM VGA). The rendered picture is cut at this line.
static int VgaVerticalDisplayEnd(const VgaDevice* d) {
  return d->cr[kCrtcVerticalDisplayEnd] | ((d->cr[kCrtcOverflow] & 0x02) << 7) |
         ((d->cr[kCrtcOverflow] & 0x40) << 3);
}

static int VgaCharDots(const VgaDevice* d) { return (d->misc & kMsrDots8) ? 8 : 9; }

static int VgaHorizontalTotal(const VgaDevice* d) {
  const int clocking9 = (d->sr[kSeqClockMode] >> 3) & 1;
  return (d->cr[kCrtcHorizontalTotal] + 5) << clocking9;
}

static void VgaUpdateStatus(VgaDevice* d, uint64_t now_us) {
  const int htotal = VgaHorizontalTotal(d);
  const int frame_lines = VgaVerticalTotal(d) + 2;
  const int vstart = VgaVerticalRetraceStart(d);
  const int vend = vstart + (d->cr[kCrtcVerticalRetraceEnd] & kField4Bits) + 1;
  const int hstart = d->cr[kCrtcStartHorizontalRetrace] +
                     ((d->cr[kCrtcEndHorizontalRetrace] >> 5) & kField2Bits);
  const int hend = hstart + (d->cr[kCrtcEndHorizontalRetrace] & kField5Bits) + 1;
  const int clock_sel = (d->misc >> kMsrClockSelShift) & kField2Bits;
  const int char_hz = kVgaDotClockHz[clock_sel] / VgaCharDots(d);
  const int frame_chars = frame_lines * htotal;
  // The bits the raster cannot decide stay as they were; only the two the frame
  // geometry defines are recomputed.
  uint8_t val = (uint8_t)(d->st01 & (uint8_t) ~(kStatusVsync | kStatusDisplayEnable));
  if (frame_chars > 0 && char_hz > 0) {
    const int64_t ns_per_char = kNsPerSecond / char_hz;
    const int64_t now_ns = (int64_t)now_us * kNsPerMicroSecond;
    const int char_in_frame = (int)((now_ns / ns_per_char) % frame_chars);
    const int line = char_in_frame / htotal;
    if (line >= vstart && line <= vend) {
      val = (uint8_t)(val | kStatusVsync | kStatusDisplayEnable);  // vertical retrace
    } else if (char_in_frame % htotal >= hstart && char_in_frame % htotal <= hend) {
      val = (uint8_t)(val | kStatusDisplayEnable);  // horizontal retrace
    }
  } else {
    // Nothing to scan from -- the state before the CRTC is programmed, where the
    // registers give the raster no frame to sit in. Alternating the two flags is
    // what such a card reads like to a guest that polls the port.
    val = (uint8_t)(d->st01 ^ (kStatusVsync | kStatusDisplayEnable));
  }
  d->st01 = val;
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

// --- DISPI (VBE's register pair) -------------------------------------------------
// The index/data ports sit outside the 0x3B0-0x3DF block, so VgaPortDecoded's
// monochrome/colour gate never sees them and they always answer: the block this
// file registers them in is separate, and the option ROM's probe must not depend
// on which half of the register file is currently decoded.
//
// A word access is one register, not two byte lanes: the ROM addresses DISPI
// with outw/inw (bochsvga.c:75-81 issues `outw reg, 0x1CE` then `inw val, 0x1CF`),
// so 0x1CE selects a register and 0x1CF is that register -- 16 bits, because the
// ids alone (0xB0C0) do not fit in a byte.
//
// Two registers do not read back what was written, and both matter:
//   - ID answers with the id the option ROM last wrote, which is the probe
//     itself (bochsvga.c writes VBE_DISPI_ID0 then reads it back)
//   - VIDEO_MEMORY_64K is read-only and reports the card's actual video memory
//     in 64KB units; the firmware turns it into the framebuffer size it
//     advertises and drops the extended modes that do not fit (bochsvga.c:347,
//     :370).
static int DispiKnownId(uint16_t v) { return v >= 0xb0c0 && v <= 0xb0c5; }

// The register file the index points into. bochsvga.h:15-25 names exactly
// kDispiRegCount registers, ID (0x0) through VIDEO_MEMORY_64K (0xa). Two of them
// answer from their own fields and the other nine are stored here; an index
// naming none of these addresses nothing. Returning a slot for it would hand the
// data port an address past the end of `dispi`, which sits directly in front of
// the PCI config space in VgaDevice -- one out-of-range index write and a guest
// would be editing the card's configuration space through 0x1CF.
static uint16_t* DispiReg(VgaDevice* d) {
  switch (d->dispi_index) {
    case kDispiXres:
    case kDispiYres:
    case kDispiBpp:
    case kDispiEnable:
    case kDispiBank:
    case kDispiVirtWidth:
    case kDispiVirtHeight:
    case kDispiXOffset:
    case kDispiYOffset:
      return &d->dispi[d->dispi_index];
    default:
      return NULL;
  }
}

static uint16_t VgaDispiReadReg(VgaDevice* d) {
  switch (d->dispi_index) {
    case kDispiId:
      return d->dispi_id;
    case kDispiVideoMemory64K:
      return (uint16_t)(kVgaVramSize / 65536);
    default:
      break;
  }
  const uint16_t* slot = DispiReg(d);
  return slot ? *slot : 0xffff;
}

static void VgaDispiWriteReg(VgaDevice* d, uint16_t val) {
  switch (d->dispi_index) {
    case kDispiId:
      // Only the six ids are accepted, and an id outside them changes nothing:
      // the register keeps the last id the card was given. Measured against the
      // reference (qemu-system-i386 -vga std answers a write of 0x1234 by
      // reading back the 0xB0C0 it already held), and it is the reading that
      // makes the ROM's probe mean something -- bochsvga.c:309-313 writes ID0
      // and compares, so a card that zeroed its id on a stray write would fail
      // its own handshake.
      if (DispiKnownId(val)) d->dispi_id = val;
      return;
    case kDispiVideoMemory64K:
      return;  // read-only: the video memory is the card's, not the guest's
    default:
      break;
  }
  uint16_t* slot = DispiReg(d);
  if (!slot) return;  // the index names no register, so nothing is written
  if (d->dispi_index == kDispiEnable) {
    // ENABLE is a byte of flags and two of its bits are not state: GET-CAPS is a
    // strobe the ROM sets to read the capabilities and then restores
    // (bochsvga.c:357, :360), and NOCLEAR is not part of what a read answers
    // with. Echoing either would make this card report an ENABLE the guest never
    // asked for.
    *slot = (uint16_t)(val & ~(kDispiGetCaps | kDispiNoClearMem) & 0xff);
    return;
  }
  *slot = val;
}

// Reserved bits read back as zero: each register keeps only the bits the chip
// implements. The bit lists are QEMU's sr_mask / gr_mask (hw/display/vga.c),
// the behavioural cross-check for this register file -- and they are not
// cosmetic: a guest that identifies the card (Windows 1.01's display driver
// among them) writes all ones and reads back, so a mask that keeps a bit QEMU
// drops, or drops a bit QEMU keeps, makes this card answer a different card
// than the reference does. The attribute controller masks its own in the write
// path (QEMU's vga_ioport_write does the same per register).
static const uint8_t kSeqWriteMask[8] = {0x03, 0x3d, 0x0f, 0x3f, 0x0e, 0x00, 0x00, 0xff};
static const uint8_t kGfxWriteMask[16] = {0x0f, 0x0f, 0x0f, 0x1f, 0x03, 0x7b, 0x0f,
                                          0x0f, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0x00, 0x00};

// Which CRTC address pair answers. IBM VGA Technical Reference: miscellaneous
// output bit 0 (IOCS16) is the CRTC's chip-select line and moves only the CRTC
// registers -- 0x3B4/0x3B5 when clear, 0x3D4/0x3D5 when set. Every other port in
// the block decodes regardless of it: the attribute controller (0x3C0/0x3C1),
// the sequencer (0x3C4/0x3C5), the feature and misc registers (0x3C2/0x3CA/
// 0x3CC), the DAC (0x3C6-0x3C9), the graphics controller (0x3CE/0x3CF) and the
// CGA latches.
//
// Both references model the older wiring instead, where the whole half switched:
// QEMU's vga_ioport_invalid treats 0x3B0-0x3BF as invalid in colour mode and
// 0x3D0-0x3DF as invalid in mono, v86 registers the two halves apart the same
// way. That is the difference the register census shows: under QEMU a firmware
// that reaches the attribute controller without first clearing IOCS16 has its
// writes dropped, and the palette it programmed never lands. Here the TR wins;
// registered in AGENTS.md §X so the divergence from the references is on purpose
// and on the record.
static int VgaPortDecoded(const VgaDevice* d, uint16_t port) {
  const int mono = (d->misc & kMiscColorEmulation) == 0;
  if (port == 0x3b4 || port == 0x3b5) return mono;
  if (port == 0x3d4 || port == 0x3d5) return !mono;
  return 1;
}

// The register file's answer to a read: the decode gate is above, so this is
// reached only for a port the card is currently decoding.
static uint8_t VgaPortReadReg(VgaDevice* d, uint16_t port);

static uint8_t VgaPortRead8(VgaDevice* d, uint16_t port) {
  if (!VgaPortDecoded(d, port)) return 0xff;
  uint8_t v = VgaPortReadReg(d, port);
  // A read returns a value rather than changing state, but it is what the guest
  // then *decides* on -- so a transcript of port writes alone cannot explain a
  // guest that programs the card differently. The read row goes to the same
  // category as the write row: between them they are the guest's whole
  // conversation with the register file, in order.
  if (DebugOn(kDbgVgaIo)) DebugVgaIo(port, v, /*is_read=*/1);
  return v;
}

static uint8_t VgaPortReadReg(VgaDevice* d, uint16_t port) {
  switch (port) {
    case kPortAttribute:  // the index, while the flip-flop is clear
      return d->ar_flip_flop == 0 ? d->ar_index : 0;
    case kPortAttributeRead: {
      int i = d->ar_index & 0x1f;
      return i < kAttrCount ? d->ar[i] : 0;
    }
    case kPortStatus0:
      // Input status 0 carries the light-pen and switch-sense inputs and the RAM
      // feedback, none of which this card has lines for: the reset value is all
      // there is to report (AGENTS.md §X, D17).
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
  if (DebugOn(kDbgVgaIo)) DebugVgaIo(port, val, /*is_read=*/0);
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
      // The address register is 8 bits wide; the register set is R0-R24, so a
      // larger value names nothing and the data path ignores it (IBM VGA).
      d->cr_index = val;
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
//
// DISPI is the exception: it is a word register bank, and the ROM addresses it
// with outw/inw (bochsvga.c:75-81), so a word there is one register rather than
// two byte lanes. The two lanes past 0x1CF are region padding, not registers --
// see kDispiPortCount -- and answer the way the block's other undefined ports
// do. A byte access to either DISPI port reaches the byte path, which has no
// case for them: it reads high and writes nothing (registered in AGENTS.md §X's
// D17, since a byte-addressing guest would not drive the card).
static uint64_t VgaPortRead(void* dev, uint64_t addr, int size) {
  VgaDevice* d = (VgaDevice*)dev;
  if (size >= 2 && addr >= kDispiIndexPort && addr < kDispiIndexPort + kDispiPortCount) {
    uint16_t v = addr == kDispiIndexPort
                     ? d->dispi_index
                     : (addr == kDispiDataPort ? VgaDispiReadReg(d) : 0xffff);
    if (DebugOn(kDbgVgaIo)) DebugVgaIo((uint16_t)addr, v, /*is_read=*/1);
    return v;
  }
  uint64_t v = 0;
  for (int i = 0; i < size; i++)
    v |= (uint64_t)VgaPortRead8(d, (uint16_t)(addr + (uint64_t)i)) << (8 * i);
  return v;
}

static void VgaPortWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  VgaDevice* d = (VgaDevice*)dev;
  if (size >= 2 && addr >= kDispiIndexPort && addr < kDispiIndexPort + kDispiPortCount) {
    uint16_t v = (uint16_t)val;
    if (addr == kDispiIndexPort) {
      // The index has to name one of the register file's kDispiRegCount
      // registers (bochsvga.h:15-25). An index naming none leaves the register
      // the card already pointed at, which is what keeps the data port from
      // ever addressing a slot the file does not have.
      if (v < kDispiRegCount) d->dispi_index = (uint8_t)v;
    } else if (addr == kDispiDataPort) {
      VgaDispiWriteReg(d, v);
    }
    if (DebugOn(kDbgVgaIo)) DebugVgaIo((uint16_t)addr, v, /*is_read=*/0);
    return;
  }
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
    // Read mode 1: the colour compare. The read map select names the plane, and
    // that plane's byte is compared with the colour compare value under the
    // colour don't-care mask: the read answers 0xff when every cared-for bit
    // matches and 0x00 when any of them differs (IBM VGA Technical Reference
    // "Read Mode 1"; QEMU's vga_mem_readb case 0x08 does the same in one line:
    // ~(compare ^ plane_byte) & dont_care). Comparing all four planes at once
    // and folding the answers together would answer about planes the guest did
    // not ask for, and the probe vga_ops.asm pins it (its rdm1 line).
    const uint32_t plane_byte = (d->latch >> (8 * (d->gr[kGfxReadMapSelect] & 3))) & 0xff;
    const uint32_t compare = d->gr[kGfxColorCompare] & 0xff;
    const uint32_t dont_care = d->gr[kGfxColorDontCare] & 0xff;
    return (uint8_t)~((plane_byte ^ compare) & dont_care);
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

// --- the PCI function -------------------------------------------------------------
// The framebuffer BAR is a plain window onto the video memory: a guest that has
// programmed a display start address through the CRTC reaches its pixels here
// without the graphics controller's rotate/mask/logic stages, which is what makes
// it a *linear* framebuffer (QEMU maps vga.vram straight onto the BAR when the
// card offers VGA_RAM_LFB, hw/display/vga-pci.c's vga_pci_realize +
// vga_init; the register-block BAR is the I/O side of the same function).
//
// The window is the card's real video memory, so the BAR is exactly as large as
// the memory is: the DISPI VIDEO_MEMORY_64K register reports the same number, and
// firmware that sizes a linear framebuffer against one is not told a size the
// card cannot back. Both come from kVgaVramSize in vga.h, whose comment records
// the paired measurements that fixed it at 16MB (the option ROM sizes the mode
// table against this number, so a card that reports less loses modes the
// reference machine keeps).
static uint64_t VgaLfbRead(void* dev, uint64_t addr, int size) {
  VgaDevice* d = (VgaDevice*)dev;
  uint64_t off = addr - d->lfb_base;
  uint64_t v = 0;
  for (int i = 0; i < size; i++)
    v |= (uint64_t)d->vram[(off + (uint64_t)i) & (kVgaVramSize - 1)] << (8 * i);
  return v;
}

static void VgaLfbWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  VgaDevice* d = (VgaDevice*)dev;
  uint64_t off = addr - d->lfb_base;
  for (int i = 0; i < size; i++)
    d->vram[(off + (uint64_t)i) & (kVgaVramSize - 1)] = (uint8_t)(val >> (8 * i));
  d->dirty = 1;
}

static const DeviceOps kVgaLfbOps = {"vga-lfb", VgaLfbRead, VgaLfbWrite};

// Region 2 of QEMU's standard-VGA function is a 4KB MMIO bar (the standard-VGA
// specification): 0x000-0x3ff is the EDID blob, 0x400-0x41f are the VGA ports
// 0x3c0-0x3df remapped 1:1. This machine has no EDID model, so those reads
// answer open bus; the port window drives the same register file the fixed
// ports do, including the vga-io transcript. A word/dword access drives its
// byte lanes in turn, like the fixed I/O path.
static uint64_t VgaBar2Read(void* dev, uint64_t addr, int size) {
  VgaDevice* d = (VgaDevice*)dev;
  uint64_t off = addr - d->bar2_base;
  uint64_t v = 0;
  for (int i = 0; i < size; i++) {
    uint64_t o = off + (uint64_t)i;
    uint8_t b = 0xff;
    if (o >= kVgaBar2PortBase && o < kVgaBar2PortBase + kVgaBar2PortCount)
      b = VgaPortRead8(d, (uint16_t)(kVgaBar2FirstPort + (o - kVgaBar2PortBase)));
    v |= (uint64_t)b << (8 * i);
  }
  return v;
}

static void VgaBar2Write(void* dev, uint64_t addr, int size, uint64_t val) {
  VgaDevice* d = (VgaDevice*)dev;
  uint64_t off = addr - d->bar2_base;
  for (int i = 0; i < size; i++) {
    uint64_t o = off + (uint64_t)i;
    if (o >= kVgaBar2PortBase && o < kVgaBar2PortBase + kVgaBar2PortCount)
      VgaPortWrite8(d, (uint16_t)(kVgaBar2FirstPort + (o - kVgaBar2PortBase)),
                    (uint8_t)(val >> (8 * i)));
  }
}

static const DeviceOps kVgaBar2Ops = {"vga-bar2", VgaBar2Read, VgaBar2Write};

// Called before VgaInit wipes the device: the bus pointer and the programmed
// window addresses live in the device, so the decode has to come off while it
// still knows where it was put. A loop because the window can have been moved
// more than once (each move must have removed its predecessor, but a reset is
// also the place where a leaked one is cleaned up).
void VgaPciDetach(VgaDevice* d) {
  if (!d->mem_bus) return;
  while (BusRemoveRegion(d->mem_bus, &kVgaLfbOps, d)) {
  }
  while (BusRemoveRegion(d->mem_bus, &kVgaBar2Ops, d)) {
  }
  d->lfb_base = 0;
  d->bar2_base = 0;
}

static void VgaPut16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)(v >> 8);
}

static void VgaPut32(uint8_t* p, uint32_t v) {
  VgaPut16(p, (uint16_t)(v & 0xffff));
  VgaPut16(p + 2, (uint16_t)(v >> 16));
}

// BAR size decoding, the same rule ide.c's MaskBar uses (PCI spec §6.2.5.1):
// the address bits below a BAR's window are read-only zero, so firmware's
// probe -- write all ones, read back -- sees the window size and alignment.
// Without this every BAR answers "4GiB" and the resource allocator places the
// windows anywhere. `type` is the read-only encoding the BAR carries in bits
// 3:0 (memory prefetch type, or 0 for the plain MMIO BAR2).
static void VgaMaskBar(uint8_t* cfg, uint32_t size, uint32_t type) {
  uint32_t v = (uint32_t)cfg[0] | ((uint32_t)cfg[1] << 8) | ((uint32_t)cfg[2] << 16) |
               ((uint32_t)cfg[3] << 24);
  v = (v & ~(size - 1)) | type;
  cfg[0] = (uint8_t)v;
  cfg[1] = (uint8_t)(v >> 8);
  cfg[2] = (uint8_t)(v >> 16);
  cfg[3] = (uint8_t)(v >> 24);
}

// A field that does not exist on this function reads back zero however it is
// written: the BAR slots between the two real ones, and the expansion-ROM
// address register (this function serves no expansion ROM -- the machine hands
// the option ROM to the guest through its own window, so there is no ROM image
// for the enable bit at 0x30 to point at). Firmware sizes every slot it finds
// non-empty, so a slot left writable would claim a window the card does not
// decode; SeaBIOS's size probe would read all ones, take that as a 16-byte
// window, round it up to its minimum and allocate four of them (its
// pci_bios_get_bar + the PCI_DEVICE_MEM_MIN floor in pci_bios_check_devices,
// fw/pciinit.c).
static void VgaZeroField(uint8_t* cfg) {
  cfg[0] = 0;
  cfg[1] = 0;
  cfg[2] = 0;
  cfg[3] = 0;
}

static uint32_t VgaConfig32(const VgaDevice* d, int off) {
  return (uint32_t)d->pci.config[off] | ((uint32_t)d->pci.config[off + 1] << 8) |
         ((uint32_t)d->pci.config[off + 2] << 16) |
         ((uint32_t)d->pci.config[off + 3] << 24);
}

// A BAR write moves the decode. The old window has to come off first: firmware
// sizes a BAR by writing all ones and restoring the address, so a mapping that
// merely adds would leave a phantom window at the sizing address, and a BAR
// written with 0 must decode nothing (PCI spec §6.2.5.1).
static void VgaBarWritten(VgaDevice* d, uint64_t base) {
  if (!d->mem_bus) {
    d->lfb_base = base;
    return;
  }
  while (BusRemoveRegion(d->mem_bus, &kVgaLfbOps, d)) {
  }
  d->lfb_base = base;
  if (base) BusAddRegion(d->mem_bus, base, kVgaVramSize, &kVgaLfbOps, d);
}

static void VgaBar2Written(VgaDevice* d, uint32_t base) {
  if (!d->mem_bus) {
    d->bar2_base = base;
    return;
  }
  while (BusRemoveRegion(d->mem_bus, &kVgaBar2Ops, d)) {
  }
  d->bar2_base = base;
  if (base) BusAddRegion(d->mem_bus, base, kVgaBar2Size, &kVgaBar2Ops, d);
}

// BAR0 is a single 32-bit address register (QEMU registers it with
// PCI_BASE_ADDRESS_MEM_PREFETCH, not the TYPE_64 flag). The low dword is
// re-masked on every write because the type bits are read-only (PCI spec
// §6.2.5.2) and a size probe writes all ones over them.
static void VgaLfbBarWritten(VgaDevice* d) {
  VgaMaskBar(&d->pci.config[kCfgBar0], kVgaVramSize, kBarTypePrefetch);
  uint32_t base = VgaConfig32(d, kCfgBar0) & ~(uint32_t)(kVgaVramSize - 1);
  VgaBarWritten(d, base);
}

static void VgaConfigWritten(void* ctx, uint8_t off, uint8_t val) {
  VgaDevice* d = (VgaDevice*)ctx;
  (void)val;
  switch (off & (uint8_t)~3u) {
    case kCfgBar0:
      VgaLfbBarWritten(d);
      break;
    case kCfgBar2: {
      // Region 2 is a 32-bit, non-prefetchable MMIO bar: its type bits are 00
      // (QEMU standard-VGA specification). Masking with type=0 keeps it a
      // memory bar; the old code declared an I/O bar and never mapped it.
      VgaMaskBar(&d->pci.config[kCfgBar2], kVgaBar2Size, 0);
      uint32_t base = VgaConfig32(d, kCfgBar2) & ~(uint32_t)(kVgaBar2Size - 1);
      VgaBar2Written(d, base);
      break;
    }
    case kCfgBar1:
    case kCfgBar3:
    case kCfgBar4:
    case kCfgBar5:
    case kCfgCardBusCis:
    case kCfgExpRomBase:
      VgaZeroField(&d->pci.config[off & (uint8_t)~3u]);
      break;
    default:
      break;
  }
}

void VgaPciInit(VgaDevice* d, Bus* mem_bus, uint8_t bus, uint8_t dev) {
  memset(&d->pci, 0, sizeof(d->pci));
  d->pci.bus = bus;
  d->pci.dev = dev;
  d->pci.func = 0;
  VgaPut16(&d->pci.config[kCfgVendor], kPciVendorStdVga);
  VgaPut16(&d->pci.config[kCfgDevice], kPciDeviceStdVga);
  d->pci.config[kCfgRevision] = kVgaPciRevision;
  d->pci.config[kCfgProgIf] = 0x00;
  // The class code is two single bytes -- subclass then base class -- and the
  // pair is what a class search matches (SeaBIOS builds its pci->class from the
  // dword at PCI_CLASS_REVISION and compares 0x0300, hw/pci_ids.h
  // PCI_CLASS_DISPLAY_VGA; hw/pcidevice.c:65-66). Writing it as a 16-bit word
  // would spill the class byte into the latency-timer field above it and the
  // device would enumerate but match no class search at all.
  d->pci.config[kCfgSubClass] = 0x00;  // VGA controller
  d->pci.config[kCfgClass] = 0x03;     // display controller
  d->pci.config[kCfgHeaderType] = 0;  // single function, type 0 header
  // BAR0: 32-bit prefetchable memory (the type bits live in the low dword;
  // QEMU registers the same BAR with PCI_BASE_ADDRESS_MEM_PREFETCH only).
  // BAR2: a 32-bit, non-prefetchable 4KB MMIO bar -- QEMU's standard-VGA
  // specification puts the EDID blob at 0x000-0x3ff and the VGA ports
  // 0x3c0-0x3df remapped at 0x400-0x41f. BAR0 reads its type bits (0x8),
  // BAR2 reads 0; neither window decodes until firmware writes an address.
  VgaPut32(&d->pci.config[kCfgBar0], kBarTypePrefetch);
  VgaPut32(&d->pci.config[kCfgBar2], 0);
  VgaPut16(&d->pci.config[kCfgSubsystemVendor], kPciVendorStdVga);
  VgaPut16(&d->pci.config[kCfgSubsystemId], kPciDeviceStdVga);
  // A display adapter raises no interrupt (PCI spec §6.2.5.2: pin 0 means the
  // device has no interrupt pin), and QEMU's stdvga likewise leaves it zero.
  d->pci.config[kCfgInterruptPin] = 0;
  d->pci.written = VgaConfigWritten;
  d->pci.ctx = d;
  d->mem_bus = mem_bus;
  d->lfb_base = 0;
  d->bar2_base = 0;
}

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
    // CRTC R9: bit 7 doubles every row (the 200-to-400 line conversion). The
    // field's low bits (R9.4-0) are the text mode character height -- IBM VGA
    // TR 3.1a gives the maximum scan line meaning only for character rows -- so
    // a graphics picture must not repeat rows on it. (QEMU's vga_draw_graphic
    // does repeat on it, `multi_scan = ((R9 & 0x1f) + 1 << double) - 1`, with
    // an "XXX: is it correct?" of its own; AGENTS.md §五 settles a reference
    // disagreement with the manual, and the manual is on this side.)
    p->multi_scan = (d->cr[kCrtcMaxScanLine] & 0x80) ? 1 : 0;
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
  // The row pitch is the width the display channel publishes, not the CRTC's
  // raw width: the buffer is kVgaFbWidth wide and a wider mode is clipped, so
  // walking it at p->width would shear the picture and write past the array.
  const int stride = d->fb_width;
  for (int row = 0; row < p->rows; row++) {
    for (int col = 0; col < p->cols; col++) {
      const int off = p->start + row * p->line_bytes + col;
      const int px = col * p->cell_width;
      if (px >= stride) break;
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
        if (y >= d->fb_height) break;
        const uint8_t bits = d->vram[glyph + line * 4];
        uint32_t* out = &d->fb[(size_t)y * stride + px];
        const uint32_t fg_color = AttrColor(d, fg);
        const uint32_t bg_color = AttrColor(d, bg);
        for (int dot = 0; dot < p->cell_width && px + dot < stride; dot++) {
          int on = dot < 8 ? (bits >> (7 - dot)) & 1 : (dup9 ? bits & 1 : 0);
          out[dot] = on ? fg_color : bg_color;
        }
        if (cursor_shown && row * p->line_bytes + col == cursor_cell && line >= cursor_first &&
            line <= cursor_last) {
          for (int dot = 0; dot < p->cell_width && px + dot < stride; dot++)
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
  // Technical Reference; the same packing QEMU's vga_draw_line2 walks). The
  // four plane bytes of one pixel byte are one dword of the interleaved planes
  // (QEMU's vga_draw_line4 reads exactly that: one dword per pixel byte, the
  // lane per plane), which is what the aperture's standard latched write path
  // produces: the guest's address is the plane offset and SR2 says which
  // planes the byte reaches, so a driver paints with write mode 2 (or with the
  // set/reset pipeline) one byte per pixel byte.
  const int cga_colors = p->shift_control == 1;
  // The CGA latches (0x3D8/0x3D9) drive the colours only in the shapes the CGA
  // itself defines: the packed two-bit modes 4/5, and the double-scanned
  // one-bit 640x200 shape of mode 6 (IBM CGA Technical Reference: the colour
  // register picks the ink and the background of those modes). A VGA-native
  // mode that enables a single plane -- mode 0x0F's 640x350 and mode 0x11's
  // 640x480 monochrome -- takes its two colours from AR0/AR1 like any other
  // planar mode (QEMU's vga_draw_line4 goes through update_palette16 for every
  // planar mode; the CGA latches are not consulted there), so keying the CGA
  // path off "one plane enabled" would mis-colour those two modes.
  const int cga_mono = !cga_colors && p->multi_scan == 1 && p->cell_height == 2;
  const int cga = cga_colors || cga_mono;
  const int blank = cga && !(d->cga_mode & kCgaModeVideoEnable);
  const int intensity = (d->cga_color & 0x10) ? 8 : 0;
  const int palette_base = (d->cga_color & 0x20) ? 3 : 2;
  const int stride = d->fb_width;
  for (int y = 0; y < d->fb_height; y++) {
    const int row = y / (p->multi_scan + 1);
    const uint32_t base = (uint32_t)(p->start + row * p->line_bytes);
    uint32_t* out = &d->fb[(size_t)y * stride];
    for (int x = 0; x < stride; x++) {
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

// --- the mode report (CEMU_DEBUG=mark, AGENTS.md §十) -------------------------
// A guest that goes wrong on a display shows it in the register file, not in an
// instruction: which mode it asked for, and what the card ended up describing.
// This emits three frameless rows per *change* of picture, so a boot's whole
// mode sequence reads as a list: the geometry, the shapes that decide how the
// picture is read (in the bit layout below), and the timing registers it came
// from.
enum {
  kVgaMarkGraphics = 1 << 0,   // AR10 bit 0: graphics rather than text
  kVgaMarkChain4 = 1 << 1,     // SR4 bit 3
  kVgaMarkOddEven = 1 << 2,    // GR5 bit 4 (the text layout)
  kVgaMarkDoubleScan = 1 << 3, // CRTC R9 bit 7
  kVgaMarkShift = 3 << 4,      // GR5 bits 6-5: the pixel packing
  kVgaMarkPlanes = 0xf << 6,   // AR12: the colour plane enable
};
static void VgaMarkMode(VgaDevice* d, const VgaPicture* p) {
  // Only on a change: a guest that leaves the mode alone must not fill the log.
  const int flags = (p->graphics ? kVgaMarkGraphics : 0) |
                    (p->chain4 ? kVgaMarkChain4 : 0) | (p->odd_even ? kVgaMarkOddEven : 0) |
                    (p->multi_scan ? kVgaMarkDoubleScan : 0) |
                    ((p->shift_control << 4) & kVgaMarkShift) |
                    ((p->planes << 6) & kVgaMarkPlanes);
  if (flags == d->marked_flags && p->width == d->marked_width &&
      p->height == d->marked_height)
    return;
  d->marked_flags = flags;
  d->marked_width = p->width;
  d->marked_height = p->height;
  DebugMark("vga-mode", p->width, p->height);
  DebugMark("vga-shapes", flags, (unsigned)p->start);
  // The timing registers the picture above came from, so a guest that programs
  // the card itself (Windows 1.01's EGA driver does not go through int10) can
  // be compared against a mode table entry: a = R9 (max scan line) with R18
  // (display end) and R7 (overflow) above it, b = R13 (the line offset) with
  // GR6 (memory mode) and GR5 (write/read mode) above it.
  DebugMark("vga-timing", d->cr[kCrtcMaxScanLine] | (d->cr[kCrtcVerticalDisplayEnd] << 8) |
                               (d->cr[kCrtcOverflow] << 16),
            d->cr[kCrtcOffset] | (d->gr[kGfxMisc] << 8) | (d->gr[kGfxMode] << 16));
}

void VgaPoll(VgaDevice* d) {
  const uint64_t now = (uint64_t)HostTimerNow();
  VgaUpdateStatus(d, now);
  // A monotone host-clock tick, deliberately: bit 0 is the blink phase and
  // bit 1 the cursor's slower one (VgaRenderText reads both), so the count has
  // to keep advancing. Deriving either bit from where the raster happens to be
  // at poll time makes the phase a sample rather than a counter -- it can stand
  // still or go backwards, and bit 1 of a one-bit value is always zero, which
  // would leave the cursor permanently shown.
  const uint32_t blink_tick = (uint32_t)(now / kBlinkTickUs);
  // The mode report runs before the render gate, not after it: the gate batches
  // guest writes into one render every kRenderIntervalUs, so a mode the guest
  // programs and abandons within one interval (a driver that sets a mode, does
  // not like what it reads back and restores the old one) would leave no trace
  // at all in the sequence the report exists to show. Poll runs every step, so
  // every register change is seen; the render stays batched.
  VgaPicture p;
  VgaComputePicture(d, &p);
  VgaMarkMode(d, &p);
  if (!d->dirty && blink_tick == d->rendered_blink_tick) return;
  if (now - d->last_render_us < kRenderIntervalUs) return;  // batch guest writes
  // The framebuffer holds the current picture's own rows back to back: the
  // display channel publishes fb_width x fb_height and fbdump writes that many
  // pixels per row, so a render that walked the buffer at a different stride
  // would shear every row of both. Both numbers below are the same picture
  // width, clamped to the buffer's maximum the same way.
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
