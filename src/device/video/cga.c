#include "device/video/cga.h"

#include <string.h>

#include "host/host.h"
#include "debug/debug.h"

// IBM CGA (阶段 3.5 片 2). Register semantics: IBM CGA Technical Reference
// and the MC6845 CRTC datasheet; rendering shapes follow the same text-mode
// pipeline as dearchap-tinyemu vga.c vga_text_refresh (glyph-by-glyph into a
// 32bpp buffer), reduced to the CGA register set.

// --- register map -----------------------------------------------------------
enum {
  kCgaCrtcRegs = 18,  // MC6845 has R0-R17; the index is 5 bits wide
};

// Mode-control latch (0x3D8) bits.
enum {
  kMode40Col = 1u << 0,       // 1 = 40x25 alphanumeric
  kModeGraphics = 1u << 1,    // 1 = graphics (not rendered, AGENTS.md D17)
  kModeVideoEnable = 1u << 3,
  kModeBlink = 1u << 5,       // attribute bit 7 blinks instead of bg intensity
};

// Cursor-start register (R10) bits, per the IBM BIOS convention: R10=0x20
// hides the cursor (INT 10h AH=01h CX=0x2000), bit 6 makes it steady.
enum { kCurOff = 0x20, kCurSteady = 0x40 };

// Status port (0x3DA) bits: 0 = display enable (1 while blanking), 3 =
// vertical retrace, 4/5 = light-pen strobe/switch latch.
enum {
  kStDispEnable = 1u << 0,
  kStVsync = 1u << 3,
  kStPenStrobe = 1u << 4,
};

// CGA 16-color RGBI palette: bit 0 blue, bit 1 green, bit 2 red, bit 3
// intensity, with the classic brown exception at index 6 (IBM CGA Technical
// Reference; the same table the VGA BIOS text attribute defaults encode).
static const uint32_t kPalette[16] = {
    0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xaa5500,
    0xaaaaaa, 0x555555, 0x5555ff, 0x55ff55, 0x55ffff, 0xff5555, 0xff55ff,
    0xffff55, 0xffffff,
};

// Raster timing model (AGENTS.md D17: approximate, not per-scanline exact):
// a frame is 262 lines at ~63.6us (16.672ms, 60 fields/s); the visible area
// is the first 200 lines; vertical retrace occupies the tail of the frame.
enum {
  kCgaFrameLines = 262,
  kCgaFrameUs = 16672,
  kCgaVisibleLines = 200,
  kCgaVblankStartLine = 250,
  kCgaLineVisibleUs = 57,  // active pixels end before the line's blanking
};

// Blink periods are field multiples (IBM CGA reference); one tick = 8 field
// periods (~133ms). Attribute blink flips every tick, the cursor every two
// (1/16 field rate). The rendered tick is stored so phase flips re-render.
enum { kCgaBlinkTickUs = 8 * kCgaFrameUs };  // 8 field periods ≈ 133ms
// Guest writes are batched into at most one render per interval.
enum { kCgaRenderIntervalUs = 4000 };

// Reset state = what the IBM BIOS POST leaves after INT 10h mode 3 (the
// mode-3 6845 parameter block from the BIOS video table): 80x25 alphanumeric,
// cursor at cell 0, start address 0, cursor scanlines 6-7.
static const uint8_t kMode3Crtc[kCgaCrtcRegs] = {
    0x71, 0x50, 0x5A, 0x0A, 0x1F, 0x06, 0x19, 0x1C, 0x02,
    0x07, 0x06, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

void CgaInit(CgaDevice* d) {
  memset(d, 0, sizeof(*d));
  memcpy(d->crtc, kMode3Crtc, sizeof(kMode3Crtc));
  d->mode = 0x28;   // 80x25 alpha, video enabled, blink on (BIOS mode-3 value)
  d->color = 0x30;  // black border, palette-select bit set (BIOS mode-3 value)
  d->vram_ram.base = kCgaVramAddr;
  d->vram_ram.size = kCgaVramSize;
  d->vram_ram.mem = d->vram;
  d->version = 1;
  d->dirty = 1;  // first poll renders frame 1
}

void CgaRegister(Bus* bus, Bus* io, CgaDevice* d) {
  // The 16KB card RAM sits inside the board RAM region; the bus resolves the
  // smaller window first (bus.c FindRegion), so guest traffic lands here.
  BusAddRamRegion(bus, kCgaVramAddr, kCgaVramSize, &kRamOps, &d->vram_ram, d->vram);
  BusAddRegion(io, 0x3D0, 0x10, &kCgaPortOps, d);
}

// --- status port ------------------------------------------------------------
static void CgaUpdateStatus(CgaDevice* d, uint64_t now) {
  uint64_t t = now % kCgaFrameUs;
  uint64_t line = t * kCgaFrameLines / kCgaFrameUs;
  uint64_t line_us = t - line * kCgaFrameUs / kCgaFrameLines;
  uint8_t st = 0;
  if (line >= kCgaVisibleLines || line_us >= kCgaLineVisibleUs) st |= kStDispEnable;
  if (line >= kCgaVblankStartLine) st |= kStVsync;
  d->status = (uint8_t)(st | (d->status & kStPenStrobe));
}

// --- rendering ---------------------------------------------------------------
static void RenderCells(CgaDevice* d, uint64_t now) {
  const int cols = d->crtc[1];                       // R1: characters per row
  const int rows = d->crtc[6];                       // R6: character rows
  const int cheight = (d->crtc[9] & 0x1f) + 1;       // R9: scanlines per cell
  const uint16_t start = (uint16_t)((d->crtc[12] << 8) | d->crtc[13]);
  const uint16_t cur = (uint16_t)((d->crtc[14] << 8) | d->crtc[15]);
  const uint64_t tick = now / kCgaBlinkTickUs;
  const int attr_off_phase = (int)(tick & 1);
  int cursor_shown = (d->crtc[10] & kCurOff) == 0;
  if (!(d->crtc[10] & kCurSteady) && ((tick >> 1) & 1)) cursor_shown = 0;
  int cur_cell = -1;
  if (cursor_shown && cols > 0 && cur >= start &&
      (uint32_t)(cur - start) < (uint32_t)(cols * rows)) {
    cur_cell = cur - start;
  }
  int cur_line_start = d->crtc[10] & 0x1f;
  int cur_line_end = d->crtc[11] & 0x1f;
  if (cur_line_end < cur_line_start) cursor_shown = 0;

  const int wide = (d->mode & kMode40Col) ? 2 : 1;  // 40-col cells are 16 dots
  int cell = 0;
  for (int row = 0; row < rows; row++) {
    for (int col = 0; col < cols; col++, cell++) {
      int word = (int)(start + cell) & 0x1fff;  // 14-bit word counter, 16KB
      uint16_t ch_attr =
          (uint16_t)(d->vram[word * 2] | (d->vram[word * 2 + 1] << 8));
      uint8_t ch = ch_attr & 0xff;
      uint8_t at = ch_attr >> 8;
      int fg = at & 0x0f;
      int bg;
      if (d->mode & kModeBlink) {
        bg = (at >> 4) & 0x07;  // bit 3 of the bg nibble is the blink flag
        if ((at & 0x80) && attr_off_phase) fg = bg;
      } else {
        bg = at >> 4;  // blink off: the nibble is background intensity
      }
      int px = col * 8 * wide;
      if (px + 8 * wide > kCgaFbWidth) continue;  // clip beyond the 80-cell fb
      const uint8_t* glyph = &kCgaFont[ch * 8];
      for (int sl = 0; sl < cheight; sl++) {
        uint8_t bits = sl < 8 ? glyph[sl] : 0;  // taller cells pad blank lines
        int py = (row * cheight + sl) * 2;      // each line drawn twice
        if (py + 1 >= kCgaFbHeight) break;
        uint32_t* line0 = &d->fb[py * kCgaFbWidth];
        uint32_t* line1 = line0 + kCgaFbWidth;
        for (int b = 0; b < 8; b++) {
          uint32_t c = (bits & (0x80u >> b)) ? kPalette[fg] : kPalette[bg];
          if (wide == 2) {
            line0[px + 2 * b] = line0[px + 2 * b + 1] = c;
            line1[px + 2 * b] = line1[px + 2 * b + 1] = c;
          } else {
            line0[px + b] = c;
            line1[px + b] = c;
          }
        }
      }
      if (cursor_shown && cur_cell == cell) {
        for (int sl = cur_line_start; sl <= cur_line_end; sl++) {
          int py = (row * cheight + sl) * 2;
          if (py + 1 >= kCgaFbHeight) break;
          uint32_t* line0 = &d->fb[py * kCgaFbWidth];
          uint32_t* line1 = line0 + kCgaFbWidth;
          for (int b = 0; b < 8 * wide; b++) {
            line0[px + b] = kPalette[fg];
            line1[px + b] = kPalette[fg];
          }
        }
      }
    }
  }
}

// The guest prints by writing VRAM; a guest whose console never touches the
// serial port (Linux 0.11) can then only be read back through the screen. The
// mirror diffs the character plane each render and emits the rows that changed
// (CEMU_DEBUG item `screen`, AGENTS.md §X) — the 80x25 text mode this machine
// runs, with the attribute byte skipped.
static char g_screen_mirror[25][81];

static void ScreenMirror(CgaDevice* d) {
  if (!DebugOn(kDbgScreen)) return;
  static int once;
  if (!once) {
    once = 1;
    DebugText("screen-run", "mirror reached");
    DebugMark("vram", d->vram[0] | (d->vram[1] << 8), d->vram[2] | (d->vram[3] << 8));
  }
  for (int row = 0; row < 25; row++) {
    char line[81];
    for (int col = 0; col < 80; col++) {
      uint8_t ch = d->vram[(row * 80 + col) * 2];
      line[col] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : ' ';
    }
    line[80] = 0;
    int len = 80;
    while (len > 0 && line[len - 1] == ' ') line[--len] = 0;
    if (strcmp(g_screen_mirror[row], line) == 0) continue;
    strcpy(g_screen_mirror[row], line);
    DebugText("screen", line);
  }
}
static void CgaRender(CgaDevice* d, uint64_t now) {
  // Graphics modes are not rendered yet (AGENTS.md D17) and a disabled video
  // pipeline paints the (unmodeled) overscan black; both show black here.
  if (!(d->mode & kModeVideoEnable) || (d->mode & kModeGraphics)) {
    memset(d->fb, 0, sizeof(d->fb));
  } else {
    RenderCells(d, now);
  }
  d->version++;
  ScreenMirror(d);
  d->dirty = 0;
}

void CgaPoll(CgaDevice* d) {
  uint64_t now = HostTimerNow();
  CgaUpdateStatus(d, now);
  uint64_t tick = now / kCgaBlinkTickUs;
  if ((d->dirty || tick != d->rendered_blink_tick) &&
      now - d->last_render_us >= kCgaRenderIntervalUs) {
    CgaRender(d, now);
    d->rendered_blink_tick = tick;
    d->last_render_us = now;
  }
}

// --- port handlers ------------------------------------------------------------
// CGA ports are byte-wide ISA devices; a 16-bit cycle drives both byte lanes
// (out 0x3D4,ax sets index 0x3D4 and data 0x3D5), so handlers decompose.
static uint8_t CgaPortRead8(CgaDevice* d, uint16_t port) {
  switch (port) {
    case 0x3D0:
    case 0x3D2:
    case 0x3D4:
      return d->crtc_index;  // A2 is not decoded: the index port is 3x-mirrored
    case 0x3D1:
    case 0x3D3:
    case 0x3D5:
      // Real 6845 data-port reads are undefined; the file is readable like
      // the VGA register file in dearchap-tinyemu vga_ioport_read (probes
      // and guest readback rely on it).
      return d->crtc_index < kCgaCrtcRegs ? d->crtc[d->crtc_index] : 0xff;
    case 0x3DA:
      return d->status;
    default:
      return 0xff;  // write-only latches (0x3D8/0x3D9, 0x3DB/0x3DC) and the
                    // undecoded 0x3D6/0x3D7/0x3DD-0x3DF float high
  }
}

static void CgaPortWrite8(CgaDevice* d, uint16_t port, uint8_t val) {
  switch (port) {
    case 0x3D0:
    case 0x3D2:
    case 0x3D4:
      d->crtc_index = val & 0x1f;  // 5-bit index (MC6845)
      break;
    case 0x3D1:
    case 0x3D3:
    case 0x3D5:
      if (d->crtc_index < kCgaCrtcRegs) {
        d->crtc[d->crtc_index] = val;
        d->dirty = 1;
      }
      break;
    case 0x3D8:
      d->mode = val;
      d->dirty = 1;
      break;
    case 0x3D9:
      d->color = val;
      d->dirty = 1;
      break;
    case 0x3DB:
      d->status &= (uint8_t)~kStPenStrobe;  // clear light-pen latch
      break;
    case 0x3DC:
      d->status |= kStPenStrobe;  // set light-pen latch
      break;
    default:
      break;  // undecoded ports drop writes
  }
}

static uint64_t CgaPortRead(void* dev, uint64_t addr, int size) {
  CgaDevice* d = (CgaDevice*)dev;
  uint64_t v = 0;
  for (int i = 0; i < size; i++)
    v |= (uint64_t)CgaPortRead8(d, (uint16_t)(addr + i)) << (8 * i);
  return v;
}

static void CgaPortWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  CgaDevice* d = (CgaDevice*)dev;
  for (int i = 0; i < size; i++)
    CgaPortWrite8(d, (uint16_t)(addr + i), (uint8_t)(val >> (8 * i)));
}

const DeviceOps kCgaPortOps = {"cga", CgaPortRead, CgaPortWrite};

// --- display channel ----------------------------------------------------------
static const uint32_t* CgaDisplayFramebuffer(void* dev) {
  return ((CgaDevice*)dev)->fb;
}

static uint32_t CgaDisplayVersion(void* dev) { return ((CgaDevice*)dev)->version; }

const DisplaySourceOps kCgaDisplayOps = {
    kCgaFbWidth, kCgaFbHeight, CgaDisplayFramebuffer, CgaDisplayVersion};
