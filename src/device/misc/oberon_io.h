#ifndef CEMU_DEVICE_MISC_OBERON_IO_H
#define CEMU_DEVICE_MISC_OBERON_IO_H

#include <stdint.h>

#include "bus/bus.h"

// The Oberon machine's peripheral block: everything above RAM in one device
// window, mirroring the reference machine's load/store funnels
// (oberon-riscv-emu src/emu/cpu.c riscv_load/riscv_store/riscv_load_io/
// riscv_store_io). Layout, offsets from the top of the 32-bit space:
//
//   [0x100000, 0xFFFFF800)  unmapped hole — reads 0, writes dropped with a
//                            mark (the reference prints and continues)
//   [0xFFFFF800, 0xFFFFFFC0) boot ROM — reads answer the ROM words, writes
//                            dropped. The reference's *data* loads here
//                            return 0 (riscv_load's IO default), but its
//                            fetch reads the ROM; this bus serves fetch and
//                            data alike, so the ROM answers both — a
//                            well-formed Oberon guest never data-reads its
//                            ROM (divergence noted in progress.md 片 37)
//   [0xFFFFFFC0, 2^32)       the register block, offsets in the enum below:
//     +0  r   millisecond counter of the emulated clock
//     +4  r   front-panel switches (bit 0 = boot from RS232); w: LED row
//     +8/+12  r/w RS232 data/status — the PCLink1 file channel
//     +16 r/w SPI data — the selected slave (the SD card is slave 1)
//     +20 r   SPI status: bit 0 rx-ready, always set; w: slave select
//     +24 r   mouse word (packed pointer + keyboard status)
//     +28 r   keyboard FIFO byte (set-2 scancodes)
//     +32 w   the guest's stack-trace instrumentation (below)
//     +40/+44 r/w the host-clipboard channel
struct OberonSdDevice;

// Register offsets from the IO block base (cpu.h IOStart layout; Kernel.Mod
// and Input.Mod name them as negative addresses).
enum {
  kOberonRegTimer = 0,     // r: milliseconds of the emulated clock
  kOberonRegSwitches = 4,  // r: front-panel switches; w: LED row
  kOberonRegSerialData = 8,
  kOberonRegSerialStat = 12,
  kOberonRegSpiData = 16,
  kOberonRegSpiCtrl = 20,  // r: status (bit 0 rx-ready, wired 1); w: select
  kOberonRegMouse = 24,    // r: packed pointer + keyboard status
  kOberonRegKey = 28,      // r: keyboard FIFO byte (set-2 scancodes)
  kOberonRegTrace = 32,    // w: the guest's stack-trace instrumentation
  kOberonRegClipCtrl = 40,
  kOberonRegClipData = 44,
};

// The mouse word (cpu.c packing; Input.Mod Mouse unpacks it):
// x bits 11:0, y bits 23:12 counted from the bottom, keys bits 26:24
// (bit 26 left, 25 middle, 24 right), bit 28 set while the keyboard FIFO
// holds a byte.
enum {
  kOberonMouseX = 0xFFF,
  kOberonMouseYShift = 12,
  kOberonMouseXYMask = 0xFFF,
  kOberonMouseKeyShift = 24,
  kOberonKeyStatusBit = 0x10000000,
  kOberonKeyFifoDepth = 16,
};

// The guest's stack-trace instrumentation (cpu.c:122-166): instrumented
// modules announce every call as 0xAA tag words carrying three filename
// bytes each, one 0xCC word naming the module and position, and a 0 word on
// return; 0xBB dumps the stack. The reference stamps each entry with the
// guest pc; a device here cannot see the CPU (AGENTS.md §七), so the dump's
// frameless mark row carries retired-instruction stamps instead — a dump
// reproduces as `--max-inst` on that row.
enum { kOberonTraceDepth = 500, kOberonTraceFileLen = 20 };

typedef struct OberonTraceEntry {
  char file[kOberonTraceFileLen];
  uint32_t pos;
  uint8_t file_pos;
} OberonTraceEntry;

struct OberonPclinkDevice;
struct OberonClipboardDevice;

typedef struct OberonIoDevice {
  uint64_t base;        // window base
  const uint32_t* rom;  // boot ROM words, fetch-visible
  uint32_t mouse;
  uint8_t key_buf[kOberonKeyFifoDepth];
  uint32_t key_cnt;
  uint32_t switches;
  uint32_t spi_selected;
  struct OberonSdDevice* sd;          // the card on SPI slave 1
  struct OberonPclinkDevice* serial;  // the RS232 backend
  struct OberonClipboardDevice* clip;
  OberonTraceEntry trace[kOberonTraceDepth];
  uint32_t trace_index;
} OberonIoDevice;

void OberonIoInit(OberonIoDevice* d, const uint32_t* rom);
// One window from `base` to the top of the 32-bit space.
void OberonIoRegister(Bus* bus, OberonIoDevice* d, uint64_t base);
void OberonIoSetSd(OberonIoDevice* d, struct OberonSdDevice* sd);
void OberonIoSetSerial(OberonIoDevice* d, struct OberonPclinkDevice* serial);
void OberonIoSetClipboard(OberonIoDevice* d, struct OberonClipboardDevice* clip);
// The front-panel switches: bit 0 makes the boot ROM load over RS232
// instead of the card (the reference's -S). The board sets them at creation.
void OberonIoSetSwitches(OberonIoDevice* d, uint32_t switches);

// The keyboard half of the input path: the board's PS/2 keyboard model (a
// set-2 byte stream) pushes here; the FIFO drops bytes when full (cpu.c
// riscv_keyboard_input).
void OberonIoKey(OberonIoDevice* d, uint8_t byte);

// The pointer half: the host delivers movement deltas in mouse counts with
// dy counting up (host.h's pointer sink); the register holds absolute
// coordinates, so the device integrates and clamps to the 12-bit field.
void OberonIoMouseMove(OberonIoDevice* d, int dx, int dy);
// Absolute position in screen pixels (the window's absolute pointer channel;
// the register already holds absolute coordinates, so this is a direct set).
void OberonIoMouseSet(OberonIoDevice* d, int x, int y);
// Button state after the event, the host sink's convention: bit 0 left,
// bit 1 right, bit 2 middle — packed into register bits 26/24/25.
void OberonIoMouseButtons(OberonIoDevice* d, int buttons);

extern const DeviceOps kOberonIoOps;

#endif  // CEMU_DEVICE_MISC_OBERON_IO_H
