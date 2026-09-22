#include "device/input/i8042.h"

#include <stdlib.h>
#include <string.h>

#include "debug/debug.h"

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

// Output-queue depth. The guest reads one byte per access to 0x60 while the
// keyboard device can hand over several in a row (a make code plus its break
// code), so the controller queues them; QEMU's pckbd uses the same 16-byte
// depth. The queue IS the output buffer: OBF means "not empty".
enum { kI8042Queue = 16 };

struct I8042State {
  uint8_t queue[kI8042Queue];
  int head, tail;
  uint8_t cmd_byte;
  uint8_t outport;
  uint8_t last_data; // last byte written to 0x60 (echoed when nothing answers)
  uint8_t expecting;
  uint8_t a2;  // address line A2 as of the last access
  int irq;     // the IRQ line's current level
};

static int QueueCount(const struct I8042State* st) {
  return (st->tail - st->head + kI8042Queue) % kI8042Queue;
}

static void QueuePush(struct I8042State* st, uint8_t v) {
  if (QueueCount(st) == kI8042Queue - 1) return;  // full: the byte is lost (D20)
  st->queue[st->tail] = v;
  st->tail = (st->tail + 1) % kI8042Queue;
}

static uint8_t QueuePop(struct I8042State* st) {
  uint8_t v = st->queue[st->head];
  st->head = (st->head + 1) % kI8042Queue;
  return v;
}

// The keyboard interrupt is a level: a byte waiting at 0x60 holds the line up
// while the command byte enables the interrupt and the keyboard is clocked
// (PC/AT Technical Reference; QEMU pckbd.c kbd_update_irq). The guest drops it
// by reading the byte or by masking the mode bit.
static void I8042SyncIrq(I8042Device* d) {
  struct I8042State* st = d->st;
  int level = QueueCount(st) > 0 && (st->cmd_byte & kModeKbdInt) &&
              !(st->cmd_byte & kModeDisKbd);
  if (level == st->irq) return;
  st->irq = level;
  if (d->set_irq) d->set_irq(d->irq_ctx, 1, level);
  if (DebugOn(kDbgMark)) DebugMark("kbd-irq", 1, level);
}

static void I8042SyncA20(I8042Device* d) {
  // Output-port bit 1 is the A20 gate; the board decides what the line does.
  if (d->set_a20) d->set_a20(d->a20_ctx, (d->st->outport & kOutPortA20) ? 1 : 0);
}

static uint8_t I8042Status(const struct I8042State* st) {
  uint8_t v = 0;
  if (QueueCount(st) > 0) v |= kStatObf;
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
      QueuePush(st, st->cmd_byte);
      break;
    case kCmdWriteCmdByte:
      st->expecting = kExpectCmdByte;
      break;
    case kCmdReadOutPort:
      QueuePush(st, st->outport);
      break;
    case kCmdWriteOutPort:
      st->expecting = kExpectOutPort;
      break;
    case kCmdSelfTest:
      QueuePush(st, 0x55);
      break;
    case kCmdInterfaceTest:
      QueuePush(st, 0x00);
      break;
    case kCmdDisableKbd:
      st->cmd_byte |= kModeDisKbd;
      break;
    case kCmdEnableKbd:
      st->cmd_byte &= (uint8_t)~kModeDisKbd;
      break;
    case kCmdDisableAux:
    case kCmdEnableAux:
      break;  // no auxiliary device on this controller (D20)
    case kCmdPulseReset:
      // 0xFE pulses the CPU reset line (PC/AT Technical Reference; QEMU
      // pckbd.c KBD_CCMD_RESET): the machine resets, so the guest's reboot
      // lands back in firmware instead of spinning on a dead controller.
      if (d->request_reset) d->request_reset(d->reset_ctx);
      break;
    default:
      // 0xf0..0xfd pulse output-port bits 0..3 (PC/AT Technical Reference);
      // QEMU pckbd.c keeps bits 2..3 and folds the low nibble in.
      if (val >= 0xf0 && val <= 0xfd) {
        st->outport = (uint8_t)((st->outport & 0x0d) | (val & 0x0f));
        I8042SyncA20(d);
      }
      break;
  }
  I8042SyncIrq(d);
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
      // A byte for the keyboard device itself. Every one is answered: the
      // firmware's probe waits for that ACK, and without it SeaBIOS's PS/2
      // setup times out and leaves the controller with the keyboard disabled —
      // IRQ1 then never comes up (D20). QEMU's ps2.c queues one ACK per command
      // and one per parameter byte, which is exactly this; the two
      // self-identifying commands answer with their extra bytes as well.
      st->last_data = val;
      if (val == 0xff) {  // reset: ACK, then power-on-reset
        QueuePush(st, 0xfa);
        QueuePush(st, 0xaa);
      } else if (val == 0xf2) {  // identify: ACK, then the device id
        QueuePush(st, 0xfa);
        QueuePush(st, 0xab);
        QueuePush(st, 0x83);
      } else if (val == 0xee) {  // echo answers itself
        QueuePush(st, 0xee);
      } else {  // 0xf0 set-scancode-set, 0xed LEDs, 0xf3 typematic, 0xf4/0xf5, ...
        QueuePush(st, 0xfa);
      }
      break;
  }
  st->expecting = kExpectNone;
  I8042SyncIrq(d);
}

static uint64_t I8042Read(void* dev, uint64_t addr, int size) {
  (void)size;
  I8042Device* d = (I8042Device*)dev;
  struct I8042State* st = d->st;
  if (addr == kI8042CmdPort) {
    st->a2 = 1;
    return I8042Status(st);
  }
  st->a2 = 0;
  if (QueueCount(st) > 0) {
    uint8_t v = QueuePop(st);
    I8042SyncIrq(d);
    return v;
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
  if (!d->st) {
    d->st = (struct I8042State*)calloc(1, sizeof(struct I8042State));
    if (!d->st) return;
  } else {
    // Re-initialising an existing controller is the machine's reset path: the
    // model state goes back to power-on without a second allocation.
    memset(d->st, 0, sizeof(*d->st));
  }
  struct I8042State* st = d->st;
  // Reset state: the command byte the firmware expects to find (keyboard
  // interrupt and translation enabled, system flag up) and the output port
  // with A20 open — see port92.h for why the gate starts open.
  st->cmd_byte = kModeKbdInt | kModeSys | kModeKcc;
  st->outport = kOutPortA20;
  d->set_a20 = NULL;
  d->a20_ctx = NULL;
  d->set_irq = NULL;
  d->irq_ctx = NULL;
  d->request_reset = NULL;
  d->reset_ctx = NULL;
}

void I8042Register(Bus* io, I8042Device* d) {
  BusAddRegion(io, kI8042DataPort, 1, &kI8042Ops, d);
  BusAddRegion(io, kI8042CmdPort, 1, &kI8042Ops, d);
}

void I8042SetA20Sink(I8042Device* d, void (*set_a20)(void* ctx, int on), void* ctx) {
  d->set_a20 = set_a20;
  d->a20_ctx = ctx;
}

void I8042SetResetSink(I8042Device* d, void (*request_reset)(void* ctx), void* ctx) {
  d->request_reset = request_reset;
  d->reset_ctx = ctx;
}

void I8042SetIrqSink(I8042Device* d, void (*set_irq)(void* ctx, int line, int level),
                     void* ctx) {
  d->set_irq = set_irq;
  d->irq_ctx = ctx;
}

void I8042KeyByte(I8042Device* d, uint8_t scancode) {
  if (DebugOn(kDbgMark)) DebugMark("kbd-byte", (int)scancode, 0);
  QueuePush(d->st, scancode);
  I8042SyncIrq(d);
}
