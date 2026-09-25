#include "device/dma/i8237.h"

#include <string.h>

#include "debug/debug.h"

// Port windows (PC/AT Technical Reference, "DMA controller ports"; QEMU
// hw/dma/i8257.c registers the same three ranges). The slave chip's registers
// live two ports apart — 0xD0/0xD2/0xD4/... — because the PC/AT puts it on
// the 16-bit side of the bus and reserves the odd port beside each register;
// the master's are one port apart at 0x08/0x09/.../0x0F.
enum {
  kMasterBase = 0x00,
  kMasterEnd = 0x0f,
  kPageBase = 0x81,
  kPageEnd = 0x8f,
  kSlaveBase = 0xc0,
  kSlaveEnd = 0xdf,
  kSlaveCtlBase = 0xd0,
  // The command/status register's offset inside a control block; the control
  // registers run 0x08..0x0F on the master and 0xD0..0xDF on the slave.
  kCtlFirst = 0x08,
  kCtlLast = 0x0f,
  // Channel register offsets inside the master's window: a channel owns two
  // ports, address then count (0x00/0x01 = channel 0).
  kChannelPorts = 2,
};

// Page-latch port to channel (PC/AT Technical Reference; the same addresses
// QEMU hw/dma/i8257.c maps as dma_page_port[]). A port in the window with no
// latch behind it answers the open bus, which is what a PC does there.
static int PageChannel(uint64_t addr) {
  static const int kMap[kPageEnd - kPageBase + 1] = {
      /* 0x81 */ 2, /* 0x82 */ 3, /* 0x83 */ 1, /* 0x84 */ -1, /* 0x85 */ -1,
      /* 0x86 */ -1, /* 0x87 */ 0, /* 0x88 */ -1, /* 0x89 */ 6, /* 0x8a */ 7,
      /* 0x8b */ 5, /* 0x8c */ -1, /* 0x8d */ -1, /* 0x8e */ -1, /* 0x8f */ 4,
  };
  return kMap[addr - kPageBase];
}

// Every two-byte register on both chips shares one flip-flop: the first
// access after a clear takes the low byte, the next the high one (Intel 8237A
// datasheet; SeaBIOS src/hw/dma.c dma_floppy clears it before each pair).
static uint16_t FlipflopWrite(I8237Device* d, uint16_t old, uint8_t v) {
  d->flipflop ^= 1;
  if (d->flipflop) return (uint16_t)((old & 0xff00) | v);
  return (uint16_t)((old & 0x00ff) | ((uint16_t)v << 8));
}

static uint8_t FlipflopRead(I8237Device* d, uint16_t v) {
  d->flipflop ^= 1;
  if (d->flipflop) return (uint8_t)(v & 0xff);
  return (uint8_t)(v >> 8);
}

// What a register access resolved to: a channel register, a control register,
// or nothing at all (the reserved ports beside the slave's registers).
typedef struct RegHit {
  int channel;   // >= 0: this channel's address (0) or count (1)
  int is_count;
  int ctl;       // kCtlFirst..kCtlLast, or -1
  int base;      // first channel of the chip the control register belongs to
} RegHit;

static int DecodePort(uint64_t addr, RegHit* hit) {
  hit->channel = -1;
  hit->is_count = 0;
  hit->ctl = -1;
  hit->base = 0;
  if (addr >= kPageBase && addr <= kPageEnd) {
    hit->channel = PageChannel(addr);
    // A page latch is not a channel register: mark it with a distinct value
    // the caller reads as "page".
    hit->is_count = -1;
    return hit->channel >= 0 ? 1 : 0;
  }
  if (addr <= kMasterEnd) {
    // 0x00-0x07 are the four channels' address/count pairs; 0x08-0x0F are the
    // chip's own command, status, mask, mode and clear registers.
    if (addr < kCtlFirst) {
      int off = (int)addr;
      hit->channel = off / kChannelPorts;
      hit->is_count = off % kChannelPorts;
    } else {
      hit->ctl = (int)addr;
    }
    return 1;
  }
  if (addr >= kSlaveBase && addr <= kSlaveEnd) {
    hit->base = kI8237MasterChannels;
    if (addr < kSlaveCtlBase) {
      int off = (int)(addr - kSlaveBase);
      if ((off & 3) != 0 && (off & 3) != 2) return 0;  // reserved port
      hit->channel = kI8237MasterChannels + (off >> 2);
      hit->is_count = (off & 3) >> 1;
    } else {
      int off = (int)(addr - kSlaveCtlBase);
      if (off & 1) return 0;  // the reserved port beside the register
      hit->ctl = kCtlFirst + (off >> 1);
    }
    return 1;
  }
  return 0;
}

static uint64_t I8237Read(void* dev, uint64_t addr, int size) {
  (void)size;
  I8237Device* d = (I8237Device*)dev;
  RegHit hit;
  if (!DecodePort(addr, &hit)) return 0xff;
  if (hit.is_count < 0) return d->ch[hit.channel].page;
  if (hit.channel >= 0) {
    I8237Channel* c = &d->ch[hit.channel];
    return FlipflopRead(d, hit.is_count ? c->count : c->addr);
  }
  if (hit.ctl == 0x08) return d->status;
  if (hit.ctl == kCtlLast) {
    // The four mask bits of the chip this register belongs to (Intel 8237A
    // datasheet: read/write mask register).
    uint8_t v = 0;
    for (int i = 0; i < kI8237MasterChannels; i++)
      if (d->ch[hit.base + i].masked) v |= (uint8_t)(1u << i);
    return v;
  }
  return 0xff;  // write-only registers read as the open bus
}

// A master clear resets the command, status and request registers, clears the
// flip-flop and masks every channel of that chip (Intel 8237A datasheet).
static void MasterClear(I8237Device* d, int base) {
  d->command = 0;
  d->status = 0;
  d->request = 0;
  d->flipflop = 0;
  for (int i = 0; i < kI8237MasterChannels; i++) {
    I8237Channel* c = &d->ch[base + i];
    c->addr = c->addr_init = 0;
    c->count = c->count_init = 0;
    c->mode = 0;
    c->masked = 1;
  }
}

static void I8237Write(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  I8237Device* d = (I8237Device*)dev;
  uint8_t v = (uint8_t)val;
  RegHit hit;
  if (!DecodePort(addr, &hit)) return;
  if (hit.is_count < 0) {
    d->ch[hit.channel].page = v;
    return;
  }
  if (hit.channel >= 0) {
    I8237Channel* c = &d->ch[hit.channel];
    // The base and current registers are programmed together: the guest
    // writes one pair of bytes and both take the value, which is what
    // autoinit reloads from later (Intel 8237A datasheet).
    if (hit.is_count) {
      c->count = c->count_init = FlipflopWrite(d, c->count, v);
      // a = the port offset, b = the register after the write: the shared
      // flip-flop's state is only visible through what lands in it.
      if (DebugOn(kDbgMark)) DebugMark("dma-reg", (int)addr, c->count);
    } else {
      c->addr = c->addr_init = FlipflopWrite(d, c->addr, v);
    }
    return;
  }
  switch (hit.ctl) {
    case 0x08:
      d->command = v;
      break;
    case 0x09:
      d->request = v;
      break;
    case 0x0a: {
      // Single-channel mask: bits 1:0 select the channel within the chip,
      // bit 2 sets (1) or clears (0) its mask bit.
      int ch = hit.base + (v & 0x03);
      d->ch[ch].masked = (v & kI8237MaskSet) != 0;
      break;
    }
    case 0x0b: {
      int ch = hit.base + (v & kI8237ModeChannel);
      d->ch[ch].mode = v;
      break;
    }
    case 0x0c:
      d->flipflop = 0;
      break;
    case 0x0d:
      MasterClear(d, hit.base);
      break;
    case 0x0e:
      for (int i = 0; i < kI8237MasterChannels; i++) d->ch[hit.base + i].masked = 0;
      break;
    case kCtlLast:
      for (int i = 0; i < kI8237MasterChannels; i++)
        d->ch[hit.base + i].masked = (v >> i) & 1;
      break;
    default:
      break;  // 0x10..0x17 of the slave's window: reserved, dropped
  }
}

static const DeviceOps kI8237Ops = {"i8237", I8237Read, I8237Write};

void I8237Init(I8237Device* d) {
  memset(d, 0, sizeof(*d));
  I8237Reset(d);
}

void I8237Reset(I8237Device* d) {
  // Power-on is a master clear on each chip: every register cleared and every
  // channel masked (Intel 8237A datasheet, Master Clear).
  MasterClear(d, 0);
  MasterClear(d, kI8237MasterChannels);
}

void I8237Register(Bus* io, I8237Device* d) {
  BusAddRegion(io, kMasterBase, kMasterEnd - kMasterBase + 1, &kI8237Ops, d);
  BusAddRegion(io, kPageBase, kPageEnd - kPageBase + 1, &kI8237Ops, d);
  BusAddRegion(io, kSlaveBase, kSlaveEnd - kSlaveBase + 1, &kI8237Ops, d);
}

void I8237SetMemBus(I8237Device* d, Bus* mem) { d->mem = mem; }

int I8237Transfer(I8237Device* d, int channel, uint8_t* buf, int len) {
  if (channel < 0 || channel >= kI8237Channels) return 0;
  if (d->command & kI8237CmdDisable) return 0;
  if (!d->mem || len <= 0) return 0;
  I8237Channel* c = &d->ch[channel];
  if (c->masked) return 0;
  int direction = c->mode & kI8237ModeDirection;
  if (direction == kI8237DirVerify) return 0;
  // a = the count register, b = mode<<8 | channel<<4 | masked: the three
  // things that decide whether and how much this transfer moves.
  if (DebugOn(kDbgMark))
    DebugMark("dma-xfer", (int)c->count, ((int)c->mode << 8) | (channel << 4) | c->masked);
  // The count register holds "bytes minus one", so the channel still owes
  // count+1 bytes; a request for more than that stops at the count, which is
  // how a device whose command asks for a whole track gets cut off at the
  // boundary the guest programmed.
  int left = (int)c->count + kI8237CountBias;
  int n = len < left ? len : left;
  if (n <= 0) return 0;
  // The page latch extends the 16-bit address but is NOT part of the counter:
  // the chip's own address wraps inside its 64KiB page, which is the PC's
  // well-known DMA boundary (Intel 8237A datasheet; SeaBIOS src/hw/dma.c
  // refuses a transfer that would cross one).
  uint32_t addr = ((uint32_t)c->page << 16) | c->addr;
  int decrement = (c->mode & kI8237ModeDecrement) != 0;
  int to_memory = direction != kI8237DirRead;
  for (int i = 0; i < n; i++) {
    uint32_t a = decrement ? addr - (uint32_t)i : addr + (uint32_t)i;
    if (to_memory)
      BusWrite(d->mem, a, 1, buf[i]);
    else
      buf[i] = (uint8_t)BusRead(d->mem, a, 1);
  }
  c->addr = (uint16_t)((addr + (decrement ? -(uint32_t)n : (uint32_t)n)) & 0xffff);
  c->count = (uint16_t)(left - n - kI8237CountBias);
  if (n == left) {
    // The channel reached terminal count: the status bit latches (the guest
    // reads it back from the command/status register) and an autoinit channel
    // reloads both registers for the next round.
    d->status |= (uint8_t)(1u << (channel & 0x03));
    if (c->mode & kI8237ModeAutoInit) {
      c->addr = c->addr_init;
      c->count = c->count_init;
    }
  }
  return n;
}
