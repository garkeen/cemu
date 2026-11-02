#include "device/input/ps2mouse.h"

#include <stdlib.h>
#include <string.h>

#include "debug/debug.h"

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
  int reported;    // the button state the last packet carried
  int dx, dy, dz;  // movement the host reported and the guest has not read
  int command;     // -1, or the command waiting for its parameter byte
  uint64_t next_sample_ns;  // when the next sample is due (0 = arm on poll)
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

// Each packet carries at most this many counts per axis. QEMU clamps every
// packet to +/-127 (hw/input/ps2.c ps2_mouse_send_packet) and ps2_mouse_sync
// splits a larger delta across packets. The PS/2 wire format also has a 9th
// bit and overflow bits for +/-256, but guest drivers (Win95's) read a packet
// outside the signed-byte range as a jump to the screen edge, and the overflow
// bits make it worse; the split keeps every packet in the range every driver
// handles.
enum { kMaxPacketDelta = 127 };

static int PacketBytes(const struct Ps2MouseState* st) {
  return st->id == kIdImps2 ? 4 : 3;
}

// One packet. Returns 0 when the controller has no room for the whole packet;
// the caller keeps its accumulator and retries later. The caller has already
// split the movement into this packet's range.
static int SendPacket(Ps2MouseDevice* d, int dx, int dy, int dz) {
  struct Ps2MouseState* st = d->st;
  if (d->can_send && !d->can_send(d->can_send_ctx, PacketBytes(st))) return 0;
  uint8_t b0 = (uint8_t)(0x08 | (st->buttons & 0x07));
  if (dx < 0) b0 |= 0x10;
  if (dy < 0) b0 |= 0x20;
  Tx(d, b0);
  Tx(d, (uint8_t)(dx & 0xff));
  Tx(d, (uint8_t)(dy & 0xff));
  // The wheel byte only exists on the 4-byte device; a plain mouse drops the
  // wheel movement (QEMU: "Just ignore the wheels if not supported").
  if (st->id == kIdImps2) Tx(d, (uint8_t)(dz & 0xff));
  return 1;
}

// Take one packet's worth from the accumulator and send it. A full controller
// queue leaves the rest for Ps2MousePoll or the next host event.
static int SendOne(Ps2MouseDevice* d) {
  struct Ps2MouseState* st = d->st;
  if (st->id != kIdImps2) st->dz = 0;  // no wheel byte to carry it
  int dx = st->dx, dy = st->dy, dz = st->dz;
  if (dx > kMaxPacketDelta)
    dx = kMaxPacketDelta;
  else if (dx < -kMaxPacketDelta)
    dx = -kMaxPacketDelta;
  if (dy > kMaxPacketDelta)
    dy = kMaxPacketDelta;
  else if (dy < -kMaxPacketDelta)
    dy = -kMaxPacketDelta;
  if (dz > kMaxPacketDelta)
    dz = kMaxPacketDelta;
  else if (dz < -kMaxPacketDelta)
    dz = -kMaxPacketDelta;
  if (!SendPacket(d, dx, dy, dz)) return 0;
  st->dx -= dx;
  st->dy -= dy;
  st->dz -= dz;
  st->reported = st->buttons;
  return 1;
}

// The device reports on its own while it is in stream mode: one packet per
// chunk of movement, plus one for a button change (PS/2 mouse protocol, stream
// mode). A large host delta needs more than one 127-count packet; the loop
// keeps sending until the accumulator is empty or the controller's output
// queue is full, exactly like QEMU's ps2_mouse_sync. In remote mode the
// movement waits for the host's 0xeb poll (QEMU's AUX_POLL).
//
// An event that reports neither is not a report: a zero-movement packet with
// unchanged buttons says nothing, and a guest waiting for another device's
// answer counts the unsolicited traffic as stray data (SeaBIOS's ps2_recvbyte
// discards it and takes its "Discarding ps2 data" path, which in this ROM
// prints into its own padding — see progress.log).
static void Flush(Ps2MouseDevice* d) {
  struct Ps2MouseState* st = d->st;
  if (!st->enabled) return;  // no reports while the device is off
  while (st->dx != 0 || st->dy != 0 || st->dz != 0 || st->buttons != st->reported) {
    if (!SendOne(d)) return;
  }
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
  st->next_sample_ns = 0;
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
    st->next_sample_ns = 0;  // apply the new period on the next poll
    Tx(d, kReplyAck);
    return;
  }
  switch (val) {
    case kCmdSetScale11:
      st->scale21 = 0;
      if (DebugOn(kDbgMark)) DebugMark("ps2-scale", 0, 0);
      Tx(d, kReplyAck);
      break;
    case kCmdSetScale21:
      st->scale21 = 1;
      if (DebugOn(kDbgMark)) DebugMark("ps2-scale", 1, 0);
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
      st->next_sample_ns = 0;
      Tx(d, kReplyAck);
      break;
    case kCmdPoll:
      // The poll is the remote-mode read: it answers with a packet built from
      // whatever movement has accumulated, enabled or not (QEMU's AUX_POLL
      // calls ps2_mouse_send_packet directly, even for the idle state). A full
      // output queue defers the packet to the next poll.
      Tx(d, kReplyAck);
      SendOne(d);
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
      st->next_sample_ns = 0;
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
      st->reported = 0;
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
  // 2:1 scaling is a property of the sample the device reports, so apply it
  // while accumulating rather than when a (possibly split) packet goes out:
  // applying it to the remainder on every retry would compound the scaling.
  if (st->scale21) {
    dx = Scale21(dx);
    dy = Scale21(dy);
  }
  st->dx += dx;
  st->dy += dy;
  st->dz += dz;
  // The packet goes out from Ps2MousePoll at the programmed sample rate; a
  // host event only updates the accumulator.
}

// Send what has accumulated since the previous sample. The board passes the
// emulated clock, so the cadence is the guest's `rate` (40/100/200 Hz), not
// the host's WM_MOUSEMOVE rate. A real PS/2 mouse samples at that rate and the
// guest's acceleration is calibrated for it; sending per host event made the
// cadence depend on how often the window pump happened to run.
void Ps2MousePoll(Ps2MouseDevice* d, uint64_t now_ns) {
  struct Ps2MouseState* st = d->st;
  if (st->remote || !st->enabled) return;
  uint64_t rate = st->rate > 0 ? (uint64_t)st->rate : (uint64_t)kDefaultRate;
  uint64_t period = 1000000000ull / rate;
  if (period == 0) period = 1;
  if (st->next_sample_ns == 0) st->next_sample_ns = now_ns + period;
  if (now_ns < st->next_sample_ns) return;
  // If the poll was late, skip the missed sample slots instead of emitting a
  // burst for each one.
  do {
    st->next_sample_ns += period;
  } while (st->next_sample_ns <= now_ns);
  Flush(d);
}

void Ps2MouseSampleNow(Ps2MouseDevice* d) {
  struct Ps2MouseState* st = d->st;
  if (st->remote || !st->enabled) return;
  Flush(d);
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
  // A reset leaves the byte sinks to the board's wiring pass, the same contract
  // I8042Init follows for its own sinks.
  d->tx = NULL;
  d->tx_ctx = NULL;
  d->can_send = NULL;
  d->can_send_ctx = NULL;
}

void Ps2MouseSetTxSink(Ps2MouseDevice* d, void (*tx)(void* ctx, uint8_t byte), void* ctx) {
  d->tx = tx;
  d->tx_ctx = ctx;
}

void Ps2MouseSetCanSendSink(Ps2MouseDevice* d, int (*can_send)(void* ctx, int bytes), void* ctx) {
  d->can_send = can_send;
  d->can_send_ctx = ctx;
}
