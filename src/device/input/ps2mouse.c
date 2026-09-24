#include "device/input/ps2mouse.h"

#include <stdlib.h>
#include <string.h>

// The command set (PS/2 mouse protocol: the "mouse" half of the same 0x60/0x64
// protocol as the keyboard; QEMU hw/input/ps2.c spells these AUX_*, v86
// src/ps2.js handles them in port60_write's mouse branch).
enum {
  kCmdSetScale11 = 0xe6,  // 1:1 scaling
  kCmdSetScale21 = 0xe7,  // 2:1 scaling
  kCmdSetRes = 0xe8,      // next byte: resolution
  kCmdGetStatus = 0xe9,   // ACK + status + resolution + sample rate
  kCmdSetStream = 0xea,   // stream mode
  kCmdPoll = 0xeb,        // ACK + one packet now
  kCmdResetWrap = 0xec,   // leave wrap mode
  kCmdSetWrap = 0xee,     // enter wrap mode (every byte is echoed back)
  kCmdSetRemote = 0xf0,   // remote mode (a packet only on 0xeb)
  kCmdGetId = 0xf2,       // ACK + device id
  kCmdSetRate = 0xf3,     // next byte: sample rate
  kCmdEnable = 0xf4,      // start reporting
  kCmdDisable = 0xf5,     // stop reporting
  kCmdSetDefaults = 0xf6,
  kCmdReset = 0xff,       // ACK + BAT + device id
};

// The device's replies (PS/2 mouse protocol).
enum { kReplyAck = 0xfa, kReplyResend = 0xfe, kReplyBat = 0xaa };

// Device ids read back by 0xf2. 0x00 is the plain 3-byte mouse; 0x03 is the
// "IMPS/2" wheel mouse the sample-rate handshake below announces, which sends
// a 4th byte per packet. IMEX (0x04, five buttons) is not a device this model
// claims, so the 200,200,80 handshake leaves the id at 0x00.
enum { kIdStandard = 0x00, kIdImps2 = 0x03 };

// The sample rates a PS/2 mouse accepts (PS/2 mouse protocol: "Correct values
// are, e.g., 10, 20, 40, 60, 80, 100, 200"). A rate outside the list draws the
// resend instead of the ACK.
static const int kRates[] = {10, 20, 40, 60, 80, 100, 200};

// The wheel handshake: a host that wants to know whether the mouse has a wheel
// sets 200, 100, 80 Hz in a row, and the 0xf2 that follows then reports ID
// 0x03. QEMU's mouse_detect_state runs the same sequence (its extra state is
// the 200,200,80 variant that announces IMEX).
enum { kDetectIdle = 0, kDetectSaw200, kDetectSaw100 };

// The power-on and 0xf6 state: 100 Hz, 4 counts/mm (code 2), stream mode,
// reporting disabled, 1:1 scaling.
enum { kDefaultRate = 100, kDefaultRes = 2 };

struct Ps2MouseState {
  int enabled;     // 0xf4/0xf5: reporting
  int remote;      // 0xf0/0xea: a packet only when the host polls (0xeb)
  int wrap;        // 0xee/0xec: every byte written is echoed back
  int scale21;     // 0xe6/0xe7: 2:1 scaling of the reported movement
  int res;         // code 0..3 -> 1, 2, 4, 8 counts/mm
  int rate;        // Hz
  int id;          // 0x00 or 0x03
  int detect;      // the wheel handshake state
  int buttons;     // bit 0 left, bit 1 right, bit 2 middle (the packet's order)
  int dx, dy, dz;  // movement the host reported and the guest has not read
  int command;     // -1, or the command waiting for its parameter byte
};

static void Tx(Ps2MouseDevice* d, uint8_t b) {
  if (d->tx) d->tx(d->tx_ctx, b);
}

// 2:1 scaling (PS/2 mouse protocol; v86 ps2.js apply_scaling2 is the same
// table). It is not a doubling: the low counts map to 1, 1, 3, 6, 9 and only
// from 6 up is it a plain doubling.
static int Scale21(int n) {
  int sign = n < 0 ? -1 : 1;
  int abs = n < 0 ? -n : n;
  switch (abs) {
    case 0:
    case 1:
    case 3: return n;
    case 2: return sign;
    case 4: return 6 * sign;
    case 5: return 9 * sign;
    default: return n * 2;
  }
}

// One packet. The counts are 9-bit two's complement — the dy8/dx8 sign bits
// plus the 8-bit fields — so the range is -256..+255, and a count outside it
// sets the matching overflow bit (PS/2 mouse protocol: "the movement in the X
// and Y direction in 9-bit two's complement notation (range -256 to +255) and
// an overflow indicator"). QEMU clamps to +-127 and never sets the overflow
// bits (hw/input/ps2.c, "XXX: increase range to 8 bits ?"); v86 truncates
// silently. This follows the protocol.
static void SendPacket(Ps2MouseDevice* d, int dx, int dy, int dz) {
  struct Ps2MouseState* st = d->st;
  uint8_t b0 = (uint8_t)(0x08 | (st->buttons & 0x07));
  if (dx < -256 || dx > 255) {
    b0 |= 0x40;
    dx = dx < 0 ? -256 : 255;
  }
  if (dy < -256 || dy > 255) {
    b0 |= 0x80;
    dy = dy < 0 ? -256 : 255;
  }
  if (dx < 0) b0 |= 0x10;
  if (dy < 0) b0 |= 0x20;
  Tx(d, b0);
  Tx(d, (uint8_t)(dx & 0xff));
  Tx(d, (uint8_t)(dy & 0xff));
  // The wheel byte only exists on the 4-byte device; a plain mouse drops the
  // wheel movement (QEMU: "Just ignore the wheels if not supported").
  if (st->id == kIdImps2) Tx(d, (uint8_t)(dz & 0xff));
}

// The device reports on its own while it is in stream mode: one packet per
// host event, carrying the movement that has accumulated since the last one.
// In remote mode the movement waits for the host's 0xeb poll (QEMU
// ps2_mouse_sync; v86 keeps the delta in mouse_delta_x until then).
static void Sync(Ps2MouseDevice* d) {
  struct Ps2MouseState* st = d->st;
  if (st->remote) return;
  int dx = st->dx, dy = st->dy;
  if (st->scale21) {
    dx = Scale21(dx);
    dy = Scale21(dy);
  }
  SendPacket(d, dx, dy, st->dz);
  st->dx = st->dy = st->dz = 0;
}

// 0xe9: the status byte, then the resolution code and the sample rate. The
// status byte carries the buttons in the opposite order from the packet's
// (bit 2 left, bit 1 middle, bit 0 right), so they are remapped here. QEMU and
// v86 both leave the button bits clear in this byte.
static void SendStatus(Ps2MouseDevice* d) {
  struct Ps2MouseState* st = d->st;
  uint8_t s = 0;
  if (st->remote) s |= 0x40;
  if (st->enabled) s |= 0x20;
  if (st->scale21) s |= 0x10;
  if (st->buttons & 0x01) s |= 0x04;  // left
  if (st->buttons & 0x02) s |= 0x01;  // right
  if (st->buttons & 0x04) s |= 0x02;  // middle
  Tx(d, s);
  Tx(d, (uint8_t)st->res);
  Tx(d, (uint8_t)st->rate);
}

static int RateValid(int rate) {
  for (size_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); i++)
    if (kRates[i] == rate) return 1;
  return 0;
}

// The wheel handshake (see the state enum above).
static void Detect(Ps2MouseDevice* d, int rate) {
  struct Ps2MouseState* st = d->st;
  switch (st->detect) {
    case kDetectIdle:
      if (rate == 200) st->detect = kDetectSaw200;
      break;
    case kDetectSaw200:
      st->detect = rate == 100 ? kDetectSaw100 : kDetectIdle;
      break;
    default:  // kDetectSaw100
      if (rate == 80) st->id = kIdImps2;
      st->detect = kDetectIdle;
      break;
  }
}

static void SetDefaults(struct Ps2MouseState* st) {
  st->enabled = 0;
  st->remote = 0;
  st->scale21 = 0;
  st->rate = kDefaultRate;
  st->res = kDefaultRes;
}

void Ps2MouseWrite(Ps2MouseDevice* d, uint8_t val) {
  struct Ps2MouseState* st = d->st;
  // Wrap mode echoes every byte back instead of acting on it; only the command
  // that leaves the mode is recognised (QEMU ps2_write_mouse's mouse_wrap
  // branch). The protocol's reset is left to the normal path below.
  if (st->wrap && val != kCmdReset) {
    if (val == kCmdResetWrap) st->wrap = 0;
    Tx(d, val);
    return;
  }
  if (st->command == kCmdSetRes) {
    st->command = -1;
    // 0..3 mean 1, 2, 4, 8 counts/mm; anything else draws the resend (PS/2
    // mouse protocol, the same "if the given value is not acceptable the NACK
    // is fe" rule the sample rate follows). QEMU stores any value unclamped.
    if (val > 3) {
      Tx(d, kReplyResend);
      return;
    }
    st->res = val;
    Tx(d, kReplyAck);
    return;
  }
  if (st->command == kCmdSetRate) {
    st->command = -1;
    if (!RateValid(val)) {
      Tx(d, kReplyResend);
      return;
    }
    st->rate = val;
    Detect(d, val);
    Tx(d, kReplyAck);
    return;
  }
  switch (val) {
    case kCmdSetScale11:
      st->scale21 = 0;
      Tx(d, kReplyAck);
      break;
    case kCmdSetScale21:
      st->scale21 = 1;
      Tx(d, kReplyAck);
      break;
    case kCmdSetRes:
    case kCmdSetRate:
      st->command = val;  // the parameter byte follows
      Tx(d, kReplyAck);
      break;
    case kCmdGetStatus:
      Tx(d, kReplyAck);
      SendStatus(d);
      break;
    case kCmdSetStream:
      st->remote = 0;
      Tx(d, kReplyAck);
      break;
    case kCmdPoll:
      // The poll is the remote-mode read: it answers with a packet built from
      // whatever movement has accumulated, enabled or not (QEMU's AUX_POLL).
      Tx(d, kReplyAck);
      SendPacket(d, st->dx, st->dy, st->dz);
      st->dx = st->dy = st->dz = 0;
      break;
    case kCmdSetWrap:
      st->wrap = 1;
      Tx(d, kReplyAck);
      break;
    case kCmdSetRemote:
      st->remote = 1;
      Tx(d, kReplyAck);
      break;
    case kCmdGetId:
      Tx(d, kReplyAck);
      Tx(d, (uint8_t)st->id);
      break;
    case kCmdEnable:
      st->enabled = 1;
      Tx(d, kReplyAck);
      break;
    case kCmdDisable:
      st->enabled = 0;
      Tx(d, kReplyAck);
      break;
    case kCmdSetDefaults:
      SetDefaults(st);
      Tx(d, kReplyAck);
      break;
    case kCmdReset:
      SetDefaults(st);
      st->id = kIdStandard;
      st->detect = kDetectIdle;
      st->buttons = 0;
      st->dx = st->dy = st->dz = 0;
      Tx(d, kReplyAck);
      Tx(d, kReplyBat);
      Tx(d, (uint8_t)st->id);
      break;
    default:
      // A PS/2 device answers every command it is given; an unknown one draws
      // the resend (QEMU's default branch). 0xec outside wrap mode lands here,
      // as it does in QEMU.
      Tx(d, kReplyResend);
      break;
  }
}

void Ps2MouseEvent(Ps2MouseDevice* d, int dx, int dy, int dz, int buttons) {
  struct Ps2MouseState* st = d->st;
  st->buttons = buttons & 0x07;
  // Reporting off means the movement is lost, the way a real mouse's movement
  // is lost while it is not reporting (v86 ps2.js mouse_send_delta returns
  // before it accumulates). The button state still lands, so enabling the
  // device and then pressing reports the press. QEMU accumulates instead and
  // reports the backlog on the next event.
  if (!st->enabled) return;
  st->dx += dx;
  st->dy += dy;
  st->dz += dz;
  Sync(d);
}

void Ps2MouseInit(Ps2MouseDevice* d) {
  if (!d->st) {
    d->st = (struct Ps2MouseState*)calloc(1, sizeof(struct Ps2MouseState));
    if (!d->st) return;
  } else {
    memset(d->st, 0, sizeof(*d->st));
  }
  SetDefaults(d->st);
  d->st->id = kIdStandard;
  d->st->command = -1;
  // A reset leaves the byte sink to the board's wiring pass, the same contract
  // I8042Init follows for its own sinks.
  d->tx = NULL;
  d->tx_ctx = NULL;
}

void Ps2MouseSetTxSink(Ps2MouseDevice* d, void (*tx)(void* ctx, uint8_t byte), void* ctx) {
  d->tx = tx;
  d->tx_ctx = ctx;
}
