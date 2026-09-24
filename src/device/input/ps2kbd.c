#include "device/input/ps2kbd.h"

#include <stdlib.h>
#include <string.h>

// The keyboard's commands (PC/AT Technical Reference; aeb scancodes-12 §12.1).
enum {
  kCmdSetLeds = 0xed,         // next byte: the LED state
  kCmdEcho = 0xee,            // answers 0xee
  kCmdSetSet = 0xf0,          // next byte: 0 = query, 1/2/3 = select
  kCmdGetId = 0xf2,           // ACK + the two ID bytes
  kCmdSetRate = 0xf3,         // next byte: repeat delay/rate
  kCmdEnable = 0xf4,          // scanning on
  kCmdDefaultsOff = 0xf5,     // set defaults, then scanning off
  kCmdDefaults = 0xf6,        // set defaults (scanning stays on)
  kCmdRepeat = 0xf7,          // set 3 only: no effect here
  kCmdMakeBreak = 0xf8,       // set 3 only
  kCmdMakeOnly = 0xf9,        // set 3 only
  kCmdRepeatBreak = 0xfa,     // set 3 only
  kCmdSomeRepeat = 0xfb,      // set 3 only, followed by set-3 scancodes
  kCmdSomeMakeBreak = 0xfc,   // set 3 only, followed by set-3 scancodes
  kCmdSomeMakeOnly = 0xfd,    // set 3 only, followed by set-3 scancodes
  kCmdReset = 0xff,           // ACK + BAT
};

enum { kReplyAck = 0xfa, kReplyResend = 0xfe, kReplyBat = 0xaa };

// The MF2 keyboard's two ID bytes (aeb scancodes-10 §10.1's "keyboardid";
// 0xab 0x83 is the MF2 ID, and a translating 8042 turns the second byte into
// 0x41 — QEMU's ps2.c hardcodes exactly that pair).
enum { kIdFirst = 0xab, kIdSecond = 0x83 };

// What a command is waiting for. 0xf0/0xf3/0xed take one parameter byte, the
// set-3 key commands take a run of them, and everything else is a one-byte
// command.
enum { kWaitNone = 0, kWaitSet, kWaitRate, kWaitLeds, kWaitSet3Keys };

// Set-1 make code -> set-2 make code: the inverse of the 8042's translation
// table, which is set 2 -> set 1. The table below was inverted from QEMU's
// hw/input/ps2.c translate_table[256] (itself the table aeb scancodes-10 §10.3
// prints, with 0x83 -> 0x41 and 0x84 -> 0x54 as the only non-identity entries
// above 0x7f), and the inversion is clean: no two set-2 codes share a set-1
// code, and the ten keys checked by hand (Esc 01<->76, '1' 02<->16,
// 'A' 1e<->1c, Enter 1c<->5a, LShift 2a<->12, space 39<->29,
// Backspace 0e<->66, Tab 0f<->0d, LCtrl 1d<->14, LAlt 38<->11) all agree.
// A slot no set-2 code maps back to stays 0; the host never sends those.
static const uint8_t kSet2FromSet1[128] = {
    0x00, 0x76, 0x16, 0x1e, 0x26, 0x25, 0x2e, 0x36,
    0x3d, 0x3e, 0x46, 0x45, 0x4e, 0x55, 0x66, 0x0d,
    0x15, 0x1d, 0x24, 0x2d, 0x2c, 0x35, 0x3c, 0x43,
    0x44, 0x4d, 0x54, 0x5b, 0x5a, 0x14, 0x1c, 0x1b,
    0x23, 0x2b, 0x34, 0x33, 0x3b, 0x42, 0x4b, 0x4c,
    0x52, 0x0e, 0x12, 0x5d, 0x1a, 0x22, 0x21, 0x2a,
    0x32, 0x31, 0x3a, 0x41, 0x49, 0x4a, 0x59, 0x7c,
    0x11, 0x29, 0x58, 0x05, 0x06, 0x04, 0x0c, 0x03,
    0x0b, 0x02, 0x0a, 0x01, 0x09, 0x77, 0x7e, 0x6c,
    0x75, 0x7d, 0x7b, 0x6b, 0x73, 0x74, 0x79, 0x69,
    0x72, 0x7a, 0x70, 0x71, 0x7f, 0x60, 0x61, 0x78,
    0x07, 0x0f, 0x17, 0x1f, 0x27, 0x2f, 0x37, 0x3f,
    0x47, 0x4f, 0x56, 0x5e, 0x08, 0x10, 0x18, 0x20,
    0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x57, 0x6f,
    0x13, 0x19, 0x39, 0x51, 0x53, 0x5c, 0x5f, 0x62,
    0x63, 0x64, 0x65, 0x67, 0x68, 0x6a, 0x6d, 0x6e,
};

struct Ps2KbdState {
  int set;        // 1 or 2 (the AT's default is 2)
  int translate;  // the controller's translation bit
  int enabled;    // 0xf4/0xf5: scanning
  int wait;       // what the next byte is for
};

static void Tx(Ps2KbdDevice* d, uint8_t b) {
  if (d->tx) d->tx(d->tx_ctx, b);
}

// The 0xe0-prefixed keys, set 1 -> set 2 (PS/2 spec; the rows of aeb
// scancodes-10 §10.6). Only the codes a PC keyboard sends are listed; anything
// else has no set-2 counterpart here and goes through unchanged.
static int Set2Extended(int c) {
  switch (c) {
    case 0x1c: return 0x5a;  // keypad Enter
    case 0x1d: return 0x14;  // right Ctrl
    case 0x35: return 0x4a;  // keypad /
    case 0x37: return 0x12;  // PrtScr
    case 0x38: return 0x11;  // right Alt
    case 0x46: return 0x7e;  // Ctrl+Break
    case 0x47: return 0x6c;  // Home
    case 0x48: return 0x75;  // Up
    case 0x49: return 0x7d;  // PgUp
    case 0x4b: return 0x6b;  // Left
    case 0x4d: return 0x74;  // Right
    case 0x4f: return 0x69;  // End
    case 0x50: return 0x72;  // Down
    case 0x51: return 0x7a;  // PgDn
    case 0x52: return 0x70;  // Insert
    case 0x53: return 0x71;  // Delete
    case 0x5b: return 0x1f;  // left Windows
    case 0x5c: return 0x27;  // right Windows
    case 0x5d: return 0x2f;  // Menu
    case 0x5e: return 0x37;  // Power
    case 0x5f: return 0x3f;  // Sleep
    case 0x63: return 0x5e;  // Wake
    default: return c;
  }
}

// The 8042's translation reaches the device's replies as well as its scancodes:
// aeb scancodes-10 §10.3's table is the identity above 0x7f except 0x83 -> 0x41
// and 0x84 -> 0x54, and over 0x01..0x03 it maps 1, 2, 3 to 0x43, 0x41, 0x3f —
// which is why a guest in translated mode reads 0x41 for "set 2" and sees the
// MF2 ID as 0xab 0x41 (QEMU's translate_table carries exactly these entries).
static uint8_t TranslateReply(const struct Ps2KbdState* st, uint8_t b) {
  if (!st->translate) return b;
  switch (b) {
    case 0x01: return 0x43;
    case 0x02: return 0x41;
    case 0x03: return 0x3f;
    case 0x83: return 0x41;
    case 0x84: return 0x54;
    default: return b;
  }
}

// One key event as the guest must see it. Set 1 (and any translated stream) is
// the host's own encoding, so it goes through unchanged — including the break
// bit and the 0xe0 prefix. Set 2 spells a break as an 0xf0 prefix and keeps the
// 0xe0 for the extended keys (aeb §10.2).
static void Emit(Ps2KbdDevice* d, int ext, int code, int up) {
  struct Ps2KbdState* st = d->st;
  if (st->translate || st->set == 1) {
    if (ext) Tx(d, 0xe0);
    Tx(d, (uint8_t)(code | (up ? 0x80 : 0)));
    return;
  }
  if (ext) Tx(d, 0xe0);
  if (up) Tx(d, 0xf0);
  Tx(d, (uint8_t)(ext ? Set2Extended(code) : kSet2FromSet1[code]));
}

void Ps2KbdWrite(Ps2KbdDevice* d, uint8_t val) {
  struct Ps2KbdState* st = d->st;
  switch (st->wait) {
    case kWaitSet:
      st->wait = kWaitNone;
      // 0 queries the current set, 1 and 2 select one. 3 is a legal set number
      // that this keyboard does not claim, so it draws the resend the same way
      // any other unacceptable parameter does (aeb §12.1's 0xed text: "Otherwise
      // a NACK is returned").
      if (val == 0) {
        Tx(d, kReplyAck);
        Tx(d, TranslateReply(st, (uint8_t)st->set));
      } else if (val == 1 || val == 2) {
        st->set = val;
        Tx(d, kReplyAck);
      } else {
        Tx(d, kReplyResend);
      }
      return;
    case kWaitRate:
      // The repeat delay and rate have no observable effect here: key repeat is
      // the host's own, not something this keyboard generates.
      st->wait = kWaitNone;
      Tx(d, kReplyAck);
      return;
    case kWaitLeds:
      // Likewise the LEDs: this machine has none, so the byte is accepted and
      // dropped. (aeb: a byte that is itself a command is ACKed and done
      // instead; that needs no branch here, since neither path keeps state.)
      st->wait = kWaitNone;
      Tx(d, kReplyAck);
      return;
    case kWaitSet3Keys:
      // The set-3 key attributes: a run of set-3 scancodes ending at the next
      // command byte, which is then handled normally (aeb §12.1 lists the
      // terminators as "ed, ee, f0, f2-ff"). Nothing is stored — those "do not
      // influence keyboard operation when the scancode set is not Set 3", and
      // this model does not claim set 3.
      if (val == kCmdSetLeds || val == kCmdEcho || val >= kCmdSetSet) break;
      Tx(d, kReplyAck);
      return;
    default:
      break;
  }
  st->wait = kWaitNone;
  switch (val) {
    case kCmdSetLeds:
      st->wait = kWaitLeds;
      Tx(d, kReplyAck);
      break;
    case kCmdEcho:
      Tx(d, kCmdEcho);
      break;
    case kCmdSetSet:
      st->wait = kWaitSet;
      Tx(d, kReplyAck);
      break;
    case kCmdGetId:
      Tx(d, kReplyAck);
      Tx(d, kIdFirst);
      Tx(d, TranslateReply(st, kIdSecond));
      break;
    case kCmdSetRate:
      st->wait = kWaitRate;
      Tx(d, kReplyAck);
      break;
    case kCmdEnable:
      st->enabled = 1;
      Tx(d, kReplyAck);
      break;
    case kCmdDefaultsOff:
      st->enabled = 0;
      Tx(d, kReplyAck);
      break;
    case kCmdDefaults:
      Tx(d, kReplyAck);
      break;
    case kCmdSomeRepeat:
    case kCmdSomeMakeBreak:
    case kCmdSomeMakeOnly:
      st->wait = kWaitSet3Keys;
      Tx(d, kReplyAck);
      break;
    case kCmdRepeat:
    case kCmdMakeBreak:
    case kCmdMakeOnly:
    case kCmdRepeatBreak:
      Tx(d, kReplyAck);  // set-3 attributes: accepted, no effect here
      break;
    case kCmdReset:
      // Reset and self-test: ACK, then the BAT byte when the test passed (aeb
      // §12.1). Scanning comes back on and the set returns to the AT default;
      // the controller's translation bit is not the keyboard's to change.
      st->set = 2;
      st->enabled = 1;
      st->wait = kWaitNone;
      Tx(d, kReplyAck);
      Tx(d, kReplyBat);
      break;
    default:
      // "Each command (other than 0xfe) is ACKed by 0xfa. Each unknown command
      // is NACKed by 0xfe" (aeb §12). 0xfe lands here too, as it does in QEMU:
      // aeb calls it "not for use by the CPU".
      Tx(d, kReplyResend);
      break;
  }
}

void Ps2KbdSetTxSink(Ps2KbdDevice* d, void (*tx)(void* ctx, uint8_t byte), void* ctx) {
  d->tx = tx;
  d->tx_ctx = ctx;
}

void Ps2KbdSetTranslate(Ps2KbdDevice* d, int on) { d->st->translate = on ? 1 : 0; }

void Ps2KbdKey(Ps2KbdDevice* d, uint32_t scan, int extended, int up) {
  struct Ps2KbdState* st = d->st;
  if (!st->enabled) return;  // 0xf5/0xf6: scanning is off
  Emit(d, extended, (int)scan, up);
}

void Ps2KbdInit(Ps2KbdDevice* d) {
  if (!d->st) {
    d->st = (struct Ps2KbdState*)calloc(1, sizeof(struct Ps2KbdState));
    if (!d->st) return;
  } else {
    memset(d->st, 0, sizeof(*d->st));
  }
  // Power-on: the AT's set 2, scanning on, and translation on — the controller's
  // command byte comes up with that bit set (i8042.c), and a real keyboard has
  // no opinion about it, so the board keeps them in step.
  d->st->set = 2;
  d->st->translate = 1;
  d->st->enabled = 1;
  d->tx = NULL;
  d->tx_ctx = NULL;
}
