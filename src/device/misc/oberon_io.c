#include "device/misc/oberon_io.h"

#include <stdio.h>
#include <string.h>

#include "debug/debug.h"
#include "device/char/oberon_pclink.h"
#include "device/misc/oberon_clipboard.h"
#include "device/storage/oberon_sd.h"
#include "host/host.h"

// Top-of-space layout (oberon-riscv-emu src/emu/cpu.h). The window reaches
// from the end of RAM to 2^32; the hole below the ROM is part of the machine
// contract, so it cannot be left off the bus (BusRead of an unmapped address
// is fatal) — the reference answers 0 and drops the write instead.
enum {
  kOberonIoBlockBase = 0xFFFFFFC0u,
  kOberonRomBase = 0xFFFFF800u,
};

static uint32_t IoRead32(OberonIoDevice* d, uint32_t off) {
  switch (off) {
    case kOberonRegTimer:
      // The reference feeds SDL's wall-clock milliseconds at frame rate
      // (sdl-main.c riscv_set_time); the emulated clock here is continuous,
      // which is the same monotonic time base at finer granularity.
      return (uint32_t)(HostTimerNow() / 1000);
    case kOberonRegSwitches:
      return d->switches;
    case kOberonRegSerialData:
      return d->serial ? OberonPclinkReadData(d->serial) : 0;
    case kOberonRegSerialStat:
      return d->serial ? OberonPclinkReadStatus(d->serial) : 0;
    case kOberonRegSpiData:
      // An absent slave reads 0xFF (cpu.c case 16).
      if (d->spi_selected == 1 && d->sd) return OberonSdRead(d->sd);
      return 255;
    case kOberonRegSpiCtrl:
      return 1;  // bit 0 rx-ready: always set (cpu.c case 20)
    case kOberonRegMouse: {
      uint32_t m = d->mouse & 0x0FFFFFFF;
      if (d->key_cnt > 0) m |= kOberonKeyStatusBit;
      return m;
    }
    case kOberonRegClipCtrl:
      return d->clip ? OberonClipboardReadControl(d->clip) : 0;
    case kOberonRegClipData:
      return d->clip ? OberonClipboardReadData(d->clip) : 0;
    case kOberonRegKey: {
      if (d->key_cnt == 0) return 0;
      uint8_t byte = d->key_buf[0];
      d->key_cnt--;
      memmove(&d->key_buf[0], &d->key_buf[1], d->key_cnt);
      return byte;
    }
    default:
      return 0;  // +32/+40/+44 and undefined: reads are silent zero
  }
}

// The +32 instrumentation protocol (cpu.c:122-166): 0 pops a frame, tag
// bytes in the top octet of the word do the rest. The reference grows
// file_pos without a bound check; module names are far under the 20-byte
// entry, and this port stops at the last usable byte instead of walking
// into the next field.
static void TraceWrite(OberonIoDevice* d, uint32_t value) {
  if (value == 0) {
    if (d->trace_index > 0) {
      d->trace_index--;
      d->trace[d->trace_index] = (OberonTraceEntry){0};
    } else if (DebugOn(kDbgMark)) {
      DebugMark("oberon-trace", 0, 0);  // pop of an empty stack
    }
    return;
  }
  switch (value >> 24) {
    case 0xAA: {
      OberonTraceEntry* e = &d->trace[d->trace_index];
      for (int i = 0; i < 3 && e->file_pos < kOberonTraceFileLen - 1; i++) {
        e->file[e->file_pos++] = (char)((value >> (8 * i)) & 0xFF);
      }
      break;
    }
    case 0xBB: {
      // Dump: one frameless mark stamps the moment (its inst=N column), one
      // whole-line screen row per frame — the event table's columns would
      // mangle them (AGENTS.md §X on screen rows).
      if (DebugOn(kDbgMark)) DebugMark("oberon-trace", (int)d->trace_index, 0);
      if (DebugOn(kDbgScreen)) {
        char line[64];
        for (uint32_t i = 0; i < d->trace_index; i++) {
          snprintf(line, sizeof(line), "module %s at position %u", d->trace[i].file,
                   d->trace[i].pos);
          DebugText("oberon-trace", line);
        }
      }
      d->trace_index = 0;
      break;
    }
    case 0xCC: {
      if (d->trace_index >= kOberonTraceDepth) {
        if (DebugOn(kDbgMark)) DebugMark("oberon-trace", -1, 0);  // stack full
        break;
      }
      OberonTraceEntry* e = &d->trace[d->trace_index];
      e->file[e->file_pos] = 0;
      e->file_pos = 0;
      e->pos = value & 0xFFFFFF;
      d->trace_index++;
      break;
    }
    default:
      if (DebugOn(kDbgMark)) DebugMark("oberon-trace", -2, (int)(value >> 24));
      break;
  }
}

static void IoWrite32(OberonIoDevice* d, uint32_t off, uint32_t value, uint64_t addr) {
  switch (off) {
    case kOberonRegSwitches:
      // LED row. The reference's kernel parks the trap code here before its
      // halt loop (Kernel.Mod Trap), so it is worth a mark.
      if (DebugOn(kDbgMark)) DebugMark("oberon-led", (int)(value & 0xFFFF), (int)(value >> 16));
      break;
    case kOberonRegSerialData:
      if (d->serial) OberonPclinkWriteData(d->serial, value);
      break;
    case kOberonRegSpiData:
      if (d->spi_selected == 1 && d->sd) OberonSdWrite(d->sd, value);
      break;
    case kOberonRegSpiCtrl:
      // Bits 1:0 select the slave; bit 2 ("fast mode") is ignored, exactly
      // as the reference decodes it (cpu.c case 20).
      d->spi_selected = value & 3;
      break;
    case kOberonRegTrace:
      TraceWrite(d, value);
      break;
    case kOberonRegClipCtrl:
      if (d->clip) OberonClipboardWriteControl(d->clip, value);
      break;
    case kOberonRegClipData:
      if (d->clip) OberonClipboardWriteData(d->clip, value);
      break;
    default:
      // Hole, ROM, and undefined registers: the reference prints and carries
      // on (riscv_store_io default).
      if (DebugOn(kDbgMark)) {
        DebugMark("oberon-io-drop", (int)(uint32_t)(addr & 0xFFFFFFFFu),
                  (int)(value & 0xFFFFFFFFu));
      }
      break;
  }
}

static uint64_t OberonIoRead(void* dev, uint64_t addr, int size) {
  OberonIoDevice* d = (OberonIoDevice*)dev;
  if (addr < kOberonRomBase) return 0;  // the hole
  if (addr < kOberonIoBlockBase) {
    // ROM: byte-wise little-endian out of the word array.
    uint64_t v = 0;
    for (int i = 0; i < size; i++) {
      uint64_t a = addr + (uint64_t)i;
      uint32_t w = d->rom[(a - kOberonRomBase) / 4];
      v |= (uint64_t)((w >> ((a & 3) * 8)) & 0xFF) << (8 * i);
    }
    return v;
  }
  // Registers: the word cell is the unit; a sub-word read extracts from it,
  // the shape of the reference's LB/LH over riscv_load (riscv.c case 0000011).
  uint64_t cell = addr & ~3ULL;
  uint32_t word = IoRead32(d, (uint32_t)(cell - kOberonIoBlockBase));
  return (word >> ((addr & 3) * 8)) & (size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF);
}

static void OberonIoWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  OberonIoDevice* d = (OberonIoDevice*)dev;
  if (addr < kOberonIoBlockBase) {
    // Hole or ROM: dropped with a mark, the reference's undefined-IO write
    // path (riscv_store_io default).
    if (DebugOn(kDbgMark)) {
      DebugMark("oberon-io-drop", (int)(uint32_t)(addr & 0xFFFFFFFFu),
                (int)(uint32_t)(val & 0xFFFFFFFFu));
    }
    return;
  }
  uint64_t cell = addr & ~3ULL;
  uint32_t off = (uint32_t)(cell - kOberonIoBlockBase);
  if (size == 4) {
    IoWrite32(d, off, (uint32_t)val, addr);
    return;
  }
  // Sub-word store: read-merge-write through the same cell, the reference's
  // SB/SH shape (riscv.c case 0100011 does its merge against riscv_load).
  uint32_t word = IoRead32(d, off);
  int shift = (int)(addr & 3) * 8;
  uint32_t mask = (size == 1 ? 0xFFu : 0xFFFFu) << shift;
  word = (word & ~mask) | (((uint32_t)val << shift) & mask);
  IoWrite32(d, off, word, addr);
}

const DeviceOps kOberonIoOps = {"oberon-io", OberonIoRead, OberonIoWrite};

// ---- input paths ---------------------------------------------------------------

void OberonIoKey(OberonIoDevice* d, uint8_t byte) {
  if (d->key_cnt < kOberonKeyFifoDepth) d->key_buf[d->key_cnt++] = byte;
}

void OberonIoMouseMove(OberonIoDevice* d, int dx, int dy) {
  int x = (int)(d->mouse & kOberonMouseX) + dx;
  int y = (int)((d->mouse >> kOberonMouseYShift) & kOberonMouseXYMask) + dy;
  if (x < 0) x = 0;
  if (x > kOberonMouseX) x = kOberonMouseX;
  if (y < 0) y = 0;
  if (y > kOberonMouseX) y = kOberonMouseX;
  d->mouse = (d->mouse & ~((uint32_t)kOberonMouseX | (uint32_t)kOberonMouseXYMask
                                               << kOberonMouseYShift)) |
             (uint32_t)x | (uint32_t)y << kOberonMouseYShift;
}

void OberonIoMouseButtons(OberonIoDevice* d, int buttons) {
  // Host: bit 0 left, 1 right, 2 middle → register: 26 left, 24 right,
  // 25 middle (cpu.c riscv_mouse_button, sdl-main.c's SDL button numbers).
  uint32_t keys = ((uint32_t)(buttons & 1) << 26) |  // left → bit 26
                  ((uint32_t)(buttons & 2) << 23) |  // right (bit 1) → bit 24
                  ((uint32_t)(buttons & 4) << 23);   // middle (bit 2) → bit 25
  d->mouse = (d->mouse & ~((uint32_t)0x7 << kOberonMouseKeyShift)) | keys;
}

// ---- lifecycle ----------------------------------------------------------------

void OberonIoInit(OberonIoDevice* d, const uint32_t* rom) {
  memset(d, 0, sizeof(*d));
  d->rom = rom;
}

void OberonIoRegister(Bus* bus, OberonIoDevice* d, uint64_t base) {
  d->base = base;
  BusAddRegion(bus, base, 0x100000000ULL - base, &kOberonIoOps, d);
}

void OberonIoSetSd(OberonIoDevice* d, struct OberonSdDevice* sd) { d->sd = sd; }

void OberonIoSetSerial(OberonIoDevice* d, struct OberonPclinkDevice* serial) {
  d->serial = serial;
}

void OberonIoSetClipboard(OberonIoDevice* d, struct OberonClipboardDevice* clip) {
  d->clip = clip;
}

void OberonIoSetSwitches(OberonIoDevice* d, uint32_t switches) { d->switches = switches; }
