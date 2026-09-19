#include "device/input/i8042.h"

#include <stdlib.h>

// The controller's two ports (IBM PC/AT Technical Reference).
enum { kI8042DataPort = 0x60, kI8042CmdPort = 0x64 };

// Status register bits (PC/AT Technical Reference; QEMU pckbd.c spells the
// same bits KBD_STAT_*). Bit 1 (IBF, input buffer full) always reads 0: the
// model consumes a written byte within the access, which is exactly what
// firmware polls for before it sends the next command.
enum {
  kStatObf = 0x01,   // output buffer full: a byte is waiting at 0x60
  kStatSysf = 0x04,  // system flag, mirrors command-byte bit 2
  kStatA2 = 0x08,    // the last port touched was 0x64 (the A2 address line)
};

// Command-byte bits the model keeps (PC/AT Technical Reference).
enum {
  kModeKbdInt = 0x01,    // keyboard interrupt (IRQ1) enabled
  kModeSys = 0x04,       // system flag -> status bit 2
  kModeDisKbd = 0x10,    // keyboard clock disabled
  kModeKcc = 0x40,       // scancode translation
};

// Commands written to 0x64 (PC/AT Technical Reference; QEMU pckbd.c
// KBD_CCMD_*).
enum {
  kCmdReadCmdByte = 0x20,
  kCmdWriteCmdByte = 0x60,
  kCmdDisableAux = 0xa7,
  kCmdEnableAux = 0xa8,
  kCmdSelfTest = 0xaa,      // answers 0x55: the POST tests passed
  kCmdInterfaceTest = 0xab, // answers 0x00: no interface error
  kCmdDisableKbd = 0xad,
  kCmdEnableKbd = 0xae,
  kCmdReadOutPort = 0xd0,
  kCmdWriteOutPort = 0xd1,
  kCmdPulseReset = 0xfe,
};

// Command 0xd1 makes the next byte written to 0x60 the output port; 0x60
// makes it the command byte.
enum { kExpectNone = 0, kExpectCmdByte, kExpectOutPort };

// Output-port bits (PC/AT Technical Reference).
enum { kOutPortA20 = 0x02 };

struct I8042State {
  uint8_t cmd_byte;
  uint8_t outport;
  uint8_t outbuf;    // the byte waiting at 0x60 while OBF is set
  uint8_t last_data; // last byte written to 0x60 (echoed when no device answers)
  uint8_t expecting;
  uint8_t a2;  // address line A2 as of the last access
  int obf;     // a byte is waiting at 0x60
};

static void I8042SyncA20(I8042Device* d) {
  // Output-port bit 1 is the A20 gate; the board decides what the line does.
  if (d->set_a20) d->set_a20(d->a20_ctx, (d->st->outport & kOutPortA20) ? 1 : 0);
}

static uint8_t I8042Status(const struct I8042State* st) {
  uint8_t v = 0;
  if (st->obf) v |= kStatObf;
  // IBF never sets: the model consumes a written byte within the access.
  if (st->cmd_byte & kModeSys) v |= kStatSysf;
  if (st->a2) v |= kStatA2;
  return v;
}

static void I8042WriteCmd(I8042Device* d, uint8_t val) {
  struct I8042State* st = d->st;
  st->a2 = 1;
  switch (val) {
    case kCmdReadCmdByte:
      st->outbuf = st->cmd_byte;
      st->obf = 1;
      break;
    case kCmdWriteCmdByte:
      st->expecting = kExpectCmdByte;
      break;
    case kCmdReadOutPort:
      st->outbuf = st->outport;
      st->obf = 1;
      break;
    case kCmdWriteOutPort:
      st->expecting = kExpectOutPort;
      break;
    case kCmdSelfTest:
      st->outbuf = 0x55;
      st->obf = 1;
      break;
    case kCmdInterfaceTest:
      st->outbuf = 0x00;
      st->obf = 1;
      break;
    case kCmdDisableKbd:
      st->cmd_byte |= kModeDisKbd;
      break;
    case kCmdEnableKbd:
      st->cmd_byte &= (uint8_t)~kModeDisKbd;
      break;
    case kCmdDisableAux:
    case kCmdEnableAux:
      break;  // no auxiliary device on this controller
    case kCmdPulseReset:
      break;  // the CPU reset line is not modeled (AGENTS.md D18)
    default:
      // 0xf0..0xfd pulse output-port bits 0..3 (PC/AT Technical Reference);
      // QEMU pckbd.c keeps bits 2..3 and folds the low nibble in.
      if (val >= 0xf0 && val <= 0xfd) {
        st->outport = (uint8_t)((st->outport & 0x0d) | (val & 0x0f));
        I8042SyncA20(d);
      }
      break;
  }
}

static void I8042WriteData(I8042Device* d, uint8_t val) {
  struct I8042State* st = d->st;
  st->a2 = 0;
  switch (st->expecting) {
    case kExpectCmdByte:
      st->cmd_byte = val;
      break;
    case kExpectOutPort:
      st->outport = val;
      I8042SyncA20(d);
      break;
    default:
      // A scancode for the keyboard device; the PS/2 slice owns that path.
      st->last_data = val;
      break;
  }
  st->expecting = kExpectNone;
}

static uint64_t I8042Read(void* dev, uint64_t addr, int size) {
  (void)size;
  struct I8042State* st = ((I8042Device*)dev)->st;
  if (addr == kI8042CmdPort) {
    st->a2 = 1;
    return I8042Status(st);
  }
  st->a2 = 0;
  if (st->obf) {
    st->obf = 0;
    return st->outbuf;
  }
  return st->last_data;
}

static void I8042Write(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  if (addr == kI8042CmdPort)
    I8042WriteCmd((I8042Device*)dev, (uint8_t)val);
  else
    I8042WriteData((I8042Device*)dev, (uint8_t)val);
}

static const DeviceOps kI8042Ops = {"i8042", I8042Read, I8042Write};

void I8042Init(I8042Device* d) {
  struct I8042State* st = (struct I8042State*)calloc(1, sizeof(struct I8042State));
  if (!st) return;
  // Reset state: the command byte the firmware expects to find (keyboard
  // interrupt and translation enabled, system flag up) and the output port
  // with A20 open — see port92.h for why the gate starts open.
  st->cmd_byte = kModeKbdInt | kModeSys | kModeKcc;
  st->outport = kOutPortA20;
  d->st = st;
  d->set_a20 = NULL;
  d->a20_ctx = NULL;
}

void I8042Register(Bus* io, I8042Device* d) {
  BusAddRegion(io, kI8042DataPort, 1, &kI8042Ops, d);
  BusAddRegion(io, kI8042CmdPort, 1, &kI8042Ops, d);
}

void I8042SetA20Sink(I8042Device* d, void (*set_a20)(void* ctx, int on), void* ctx) {
  d->set_a20 = set_a20;
  d->a20_ctx = ctx;
}
