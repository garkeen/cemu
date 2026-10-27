#include <stdlib.h>
#include <string.h>

#include "board/board.h"
#include "board/oberon_rom.h"
#include "device/char/oberon_pclink.h"
#include "device/input/ps2kbd.h"
#include "device/misc/oberon_clipboard.h"
#include "device/misc/oberon_io.h"
#include "device/storage/oberon_sd.h"
#include "device/video/ramfb.h"
#include "util/log.h"

// The Project Oberon machine — solbjorg's RISC-V port as specified by its
// reference emulator (oberon-riscv-emu): flat RV32IM bare metal, no CSRs, no
// interrupts, no paging; every device is polled through the register block at
// the top of the address space. The contract:
//   src/emu/cpu.h    memory layout (1MB RAM, frame at 0xE7F00, ROM on top),
//   src/emu/cpu.c    the IO register block,
//   src/emu/riscv.c  fetch and reset (pc = ROM base).
// The CPU is the stock riscv32 model; the boot ROM pulls the Oberon inner
// core off the SD card (-dsk) and enters it, planting the memory constants
// Kernel.Init reads, which is why the RAM size is fixed.
static const uint64_t kRamBase = 0;
static const uint64_t kRamSize = 0x100000;             // DefaultMemSize
static const uint64_t kFrameBase = 0xE7F00;            // Display.Mod: 0E7F00H
static const int kFrameWidth = 1024, kFrameHeight = 768;
static const uint64_t kUpperBase = 0x100000;           // the IO window starts here
static const uint64_t kResetPc = 0xFFFFF800;           // ROMStart
// The reference front end's palette (sdl-main.c:21). Matching it makes an
// fbdump PPM byte-comparable with a screenshot of the same desktop.
static const uint32_t kInkOn = 0xfdf6e3;   // set bit: white
static const uint32_t kInkOff = 0x657b83;  // clear bit: black

typedef struct OberonBoard {
  Board base;
  OberonIoDevice io;
  OberonSdDevice sd;
  OberonPclinkDevice pclink;
  OberonClipboardDevice clip;
  RamfbDevice fb;
  Ps2KbdDevice kbd;
} OberonBoard;

// The machine's keyboard is a set-2 PS/2 stream: Input.Mod decodes the 0xF0
// break prefix and a set-2 code table. The host speaks set 1, which is what
// the PS/2 keyboard model converts — with the controller translation off its
// byte stream is the keyboard's native set 2 (ps2kbd.h), so the board pours
// it straight into the machine's FIFO.
static void OnKbdByte(void* ctx, uint8_t byte) { OberonIoKey((OberonIoDevice*)ctx, byte); }

static void OnHostKey(void* ctx, uint32_t scan, int extended, int up) {
  Ps2KbdKey((Ps2KbdDevice*)ctx, scan, extended, up);
}

// The window now uses the absolute pointer channel: source-picture pixels map
// straight onto the machine's absolute mouse register (the reference frontend
// does the same). The relative sink remains for CEMU_DEBUG=mouse= synthetic
// events; the 3/2 gain there keeps a short synthetic delta visible on the
// 1024x768 field.
enum { kOberonPointerGainNum = 3, kOberonPointerGainDen = 2 };

static void OnHostMouse(void* ctx, int dx, int dy, int dz, int buttons) {
  (void)dz;
  OberonIoDevice* io = (OberonIoDevice*)ctx;
  OberonIoMouseMove(io, dx * kOberonPointerGainNum / kOberonPointerGainDen,
                    dy * kOberonPointerGainNum / kOberonPointerGainDen);
  OberonIoMouseButtons(io, buttons);
}

static void OnHostMouseAbs(void* ctx, int x, int y, int dz, int buttons) {
  (void)dz;
  OberonIoDevice* io = (OberonIoDevice*)ctx;
  // The register's Y grows upward; the window's source picture Y grows down.
  OberonIoMouseSet(io, x, kFrameHeight - 1 - y);
  OberonIoMouseButtons(io, buttons);
}

static void OberonDestroy(Board* m) {
  OberonBoard* ob = (OberonBoard*)m;
  OberonSdDestroy(&ob->sd);
  OberonPclinkDestroy(&ob->pclink);
  OberonClipboardDestroy(&ob->clip);
  RamfbDestroy(&ob->fb);
}

Board* OberonBoardCreate(const BoardOpts* opts) {
  if (opts->ram_base && opts->ram_base != kRamBase) {
    LogError("oberon machine RAM is fixed at 0");
    return NULL;
  }
  if (opts->ram_size && opts->ram_size != kRamSize) {
    LogError("oberon machine RAM is fixed at 1MB (the boot ROM plants the "
             "memory limit for this size)");
    return NULL;
  }
  OberonBoard* ob = (OberonBoard*)calloc(1, sizeof(OberonBoard));
  if (!ob) return NULL;
  Board* m = &ob->base;
  m->name = "oberon";

  m->ram = RamCreate(kRamBase, kRamSize);
  if (!m->ram) {
    free(ob);
    return NULL;
  }
  BusAddRamRegion(&m->bus, kRamBase, kRamSize, &kRamOps, m->ram, m->ram->mem);

  // The display: a monochrome frame inside RAM. One bit per pixel, bit 0 of a
  // word leftmost, word line 0 at the bottom — the Oberon raster order
  // (Display.Mod's addressing against sdl-main.c's update_texture flip).
  if (RamfbInit(&ob->fb, kFrameWidth, kFrameHeight, 1, 1, kInkOn, kInkOff) != 0) {
    OberonDestroy(m);
    RamDestroy(m->ram);
    free(ob);
    return NULL;
  }
  RamfbRegister(&m->bus, &ob->fb, kFrameBase);

  // The disk on SPI slave 1 (Kernel.Mod CARD0 = 1) and the peripheral block
  // above RAM, which also serves ROM fetches.
  if (OberonSdInit(&ob->sd, opts->dsk) != 0) {
    OberonDestroy(m);
    RamDestroy(m->ram);
    free(ob);
    return NULL;
  }
  OberonIoInit(&ob->io, kOberonRom);
  OberonIoSetSd(&ob->io, &ob->sd);
  // The reference's default serial is the PCLink file channel and its
  // clipboard is the host's (sdl-main.c wires both before parsing options).
  OberonPclinkInit(&ob->pclink);
  OberonIoSetSerial(&ob->io, &ob->pclink);
  OberonClipboardInit(&ob->clip);
  OberonIoSetClipboard(&ob->io, &ob->clip);
  OberonIoSetSwitches(&ob->io, opts->switches);
  OberonIoRegister(&m->bus, &ob->io, kUpperBase);

  Ps2KbdInit(&ob->kbd);
  Ps2KbdSetTranslate(&ob->kbd, 0);
  Ps2KbdSetTxSink(&ob->kbd, OnKbdByte, &ob->io);
  m->key_in = OnHostKey;
  m->key_ctx = &ob->kbd;
  m->mouse_in = OnHostMouse;
  m->mouse_ctx = &ob->io;
  m->mouse_abs = OnHostMouseAbs;
  m->mouse_abs_ctx = &ob->io;

  m->display_dev = &ob->fb;
  m->display_ops = &kRamfbDisplayOps;

  m->default_isa = "riscv32";  // the ROM names the machine's CPU
  m->reset_pc = kResetPc;
  m->bin_htif = 0;
  m->destroy = OberonDestroy;
  m->cpu.halted = kCpuRunning;
  m->cpu.bus = &m->bus;
  return m;
}
