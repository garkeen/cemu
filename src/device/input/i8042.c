#include "device/input/i8042.h"

#include <stdlib.h>
#include <string.h>

#include "debug/debug.h"

// The controller's two ports (IBM PC/AT Technical Reference).
enum { kI8042DataPort = 0x60, kI8042CmdPort = 0x64 };

// The two interrupt lines the controller drives: the keyboard's IRQ1 and the
// auxiliary port's IRQ12 (IBM PC/AT Technical Reference).
enum { kI8042KbdIrq = 1, kI8042AuxIrq = 12 };

// Status register bits (PC/AT Technical Reference; QEMU pckbd.c spells the
// same bits KBD_STAT_*). Bit 1 (IBF, input buffer full) always reads 0: the
// model consumes a written byte within the access, which is exactly what
// firmware polls for before it sends the next command.
enum {
  kStatObf = 0x01,     // output buffer full: a byte is waiting at 0x60
  kStatSysf = 0x04,    // system flag, mirrors command-byte bit 2
  kStatA2 = 0x08,      // the last port touched was 0x64 (the A2 address line)
  kStatAuxObf = 0x20,  // the byte waiting at 0x60 came from the auxiliary port
};

// Command-byte bits the model keeps (PC/AT Technical Reference).
enum {
  kModeKbdInt = 0x01,    // keyboard interrupt (IRQ1) enabled
  kModeAuxInt = 0x02,    // auxiliary interrupt (IRQ12) enabled
  kModeSys = 0x04,       // system flag -> status bit 2
  kModeDisKbd = 0x10,    // keyboard clock disabled
  kModeDisAux = 0x20,    // auxiliary interface disabled
  kModeKcc = 0x40,       // scancode translation
};

// Commands written to 0x64 (PC/AT Technical Reference; QEMU pckbd.c
// KBD_CCMD_*).
enum {
  kCmdReadCmdByte = 0x20,
  kCmdWriteCmdByte = 0x60,
  kCmdDisableAux = 0xa7,
  kCmdEnableAux = 0xa8,
  kCmdTestAux = 0xa9,
  kCmdSelfTest = 0xaa,      // answers 0x55: the POST tests passed
  kCmdInterfaceTest = 0xab, // answers 0x00: no interface error
  kCmdDisableKbd = 0xad,
  kCmdEnableKbd = 0xae,
  kCmdReadOutPort = 0xd0,
  kCmdWriteOutPort = 0xd1,
  kCmdWriteAux = 0xd4,      // the next 0x60 write goes to the auxiliary device
  kCmdPulseReset = 0xfe,
};

// Command 0xd1 makes the next byte written to 0x60 the output port; 0x60
// makes it the command byte; 0xd4 routes it to the auxiliary device.
enum { kExpectNone = 0, kExpectCmdByte, kExpectOutPort, kExpectAuxData };

// Output-port bits (PC/AT Technical Reference).
enum { kOutPortA20 = 0x02 };

// Output-queue depth. The guest reads one byte per access to 0x60 while a
// device can hand over several in a row (a make code plus its break code, or a
// mouse packet), so the controller queues them; QEMU's pckbd uses the same
// 16-byte depth. The queue IS the output buffer: OBF means "not empty".
enum { kI8042Queue = 16 };

// One device's output queue. The controller has two — the keyboard's and the
// auxiliary port's — but one output buffer, so only one of them is on offer at
// a time (see AuxOnOffer).
typedef struct I8042Queue {
  uint8_t buf[kI8042Queue];
  int head, tail;
} I8042Queue;

struct I8042State {
  I8042Queue kbd;
  I8042Queue aux;
  uint8_t cmd_byte;
  uint8_t outport;
  uint8_t last_data; // last byte written to 0x60 (echoed when nothing answers)
  uint8_t expecting;
  uint8_t a2;  // address line A2 as of the last access
  int irq1;    // the keyboard IRQ line's current level
  int irq12;   // the auxiliary IRQ line's current level
};

static int QueueCount(const I8042Queue* q) {
  return (q->tail - q->head + kI8042Queue) % kI8042Queue;
}

static void QueuePush(I8042Queue* q, uint8_t v) {
  if (QueueCount(q) == kI8042Queue - 1) return;  // full: the byte is lost
  q->buf[q->tail] = v;
  q->tail = (q->tail + 1) % kI8042Queue;
}

static uint8_t QueuePop(I8042Queue* q) {
  uint8_t v = q->buf[q->head];
  q->head = (q->head + 1) % kI8042Queue;
  return v;
}

// Which device's byte the guest gets from 0x60. The controller has one output
// buffer, so only one queue is on offer at a time, and the keyboard wins:
// both references give it priority (QEMU pckbd.c kbd_update_irq's obsrc chain
// picks the keyboard before the mouse; v86 src/ps2.js "Kbd has priority over
// aux").
static int AuxOnOffer(const struct I8042State* st) {
  return QueueCount(&st->kbd) == 0 && QueueCount(&st->aux) > 0;
}

// Each interrupt is a level, and it belongs to whichever device's byte is on
// offer: a waiting keyboard byte holds IRQ1 up, a waiting mouse byte IRQ12,
// gated by the command byte's interrupt enables (PC/AT Technical Reference;
// QEMU pckbd.c kbd_update_irq_lines). The guest drops a line by reading the
// byte or by masking the mode bit. Command 0xa7 needs no gate here: it stops
// the bytes at the wire (see I8042AuxByte), so nothing is left to ask for the
// line.
static void I8042SyncIrq(I8042Device* d) {
  struct I8042State* st = d->st;
  int kbd = 0, aux = 0;
  if (QueueCount(&st->kbd) > 0 || QueueCount(&st->aux) > 0) {
    if (AuxOnOffer(st))
      aux = (st->cmd_byte & kModeAuxInt) ? 1 : 0;
    else
      kbd = (st->cmd_byte & kModeKbdInt) && !(st->cmd_byte & kModeDisKbd);
  }
  if (kbd != st->irq1) {
    st->irq1 = kbd;
    if (d->set_irq) d->set_irq(d->irq_ctx, kI8042KbdIrq, kbd);
    if (DebugOn(kDbgMark)) DebugMark("kbd-irq", kI8042KbdIrq, kbd);
  }
  if (aux != st->irq12) {
    st->irq12 = aux;
    if (d->set_irq) d->set_irq(d->irq_ctx, kI8042AuxIrq, aux);
    if (DebugOn(kDbgMark)) DebugMark("aux-irq", kI8042AuxIrq, aux);
  }
}

static void I8042SyncA20(I8042Device* d) {
  // Output-port bit 1 is the A20 gate; the board decides what the line does.
  if (d->set_a20) d->set_a20(d->a20_ctx, (d->st->outport & kOutPortA20) ? 1 : 0);
}

static uint8_t I8042Status(const struct I8042State* st) {
  uint8_t v = 0;
  if (QueueCount(&st->kbd) > 0 || QueueCount(&st->aux) > 0) v |= kStatObf;
  // IBF never sets: the model consumes a written byte within the access.
  if (st->cmd_byte & kModeSys) v |= kStatSysf;
  if (st->a2) v |= kStatA2;
  // Bit 5 says which device's byte is waiting, so the guest's mouse driver
  // knows to read a packet from 0x60 rather than a keystroke (PC/AT Technical
  // Reference; QEMU KBD_STAT_MOUSE_OBF, v86's next_byte_is_aux).
  if (AuxOnOffer(st)) v |= kStatAuxObf;
  return v;
}

static void I8042WriteCmd(I8042Device* d, uint8_t val) {
  struct I8042State* st = d->st;
  st->a2 = 1;
  switch (val) {
    case kCmdReadCmdByte:
      QueuePush(&st->kbd, st->cmd_byte);
      break;
    case kCmdWriteCmdByte:
      st->expecting = kExpectCmdByte;
      break;
    case kCmdReadOutPort:
      QueuePush(&st->kbd, st->outport);
      break;
    case kCmdWriteOutPort:
      st->expecting = kExpectOutPort;
      break;
    case kCmdSelfTest:
      QueuePush(&st->kbd, 0x55);
      break;
    case kCmdInterfaceTest:
      QueuePush(&st->kbd, 0x00);
      break;
    case kCmdTestAux:
      QueuePush(&st->kbd, 0x00);  // no interface error (QEMU KBD_CCMD_TEST_MOUSE)
      break;
    case kCmdDisableKbd:
      st->cmd_byte |= kModeDisKbd;
      break;
    case kCmdEnableKbd:
      st->cmd_byte &= (uint8_t)~kModeDisKbd;
      break;
    case kCmdDisableAux:
      st->cmd_byte |= kModeDisAux;
      break;
    case kCmdEnableAux:
      st->cmd_byte &= (uint8_t)~kModeDisAux;
      break;
    case kCmdWriteAux:
      // The next byte written to 0x60 is the auxiliary device's (PC/AT
      // Technical Reference; QEMU KBD_CCMD_WRITE_MOUSE).
      st->expecting = kExpectAuxData;
      break;
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
      // The translation bit is the controller's, but what changes is the
      // keyboard's output encoding, so the device hears about it.
      if (d->set_translate) d->set_translate(d->translate_ctx, (val & kModeKcc) ? 1 : 0);
      break;
    case kExpectOutPort:
      st->outport = val;
      I8042SyncA20(d);
      break;
    case kExpectAuxData:
      // A byte for the auxiliary device: it goes down the mouse wire, and
      // whatever the device answers (its ACK, its replies, a packet) comes
      // back through I8042AuxByte into the AUX queue. With the interface shut
      // down the wire is dead in this direction too.
      if (!(st->cmd_byte & kModeDisAux) && d->write_aux)
        d->write_aux(d->write_aux_ctx, val);
      break;
    default:
      // A byte for the keyboard device: ps2kbd.c owns its command set, its
      // ACKs and its scancodes, exactly as ps2mouse.c owns the auxiliary
      // device's.
      st->last_data = val;
      if (d->write_kbd) d->write_kbd(d->write_kbd_ctx, val);
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
  if (QueueCount(&st->kbd) > 0) {
    uint8_t v = QueuePop(&st->kbd);
    I8042SyncIrq(d);
    return v;
  }
  if (QueueCount(&st->aux) > 0) {
    uint8_t v = QueuePop(&st->aux);
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
  // Reset state: the command byte the firmware expects to find (both interrupt
  // enables, the system flag and translation up) and the output port with A20
  // open — see port92.h for why the gate starts open.
  st->cmd_byte = kModeKbdInt | kModeAuxInt | kModeSys | kModeKcc;
  st->outport = kOutPortA20;
  d->write_kbd = NULL;
  d->write_kbd_ctx = NULL;
  d->set_translate = NULL;
  d->translate_ctx = NULL;
  d->write_aux = NULL;
  d->write_aux_ctx = NULL;
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

void I8042SetAuxSink(I8042Device* d, void (*write_aux)(void* ctx, uint8_t byte), void* ctx) {
  d->write_aux = write_aux;
  d->write_aux_ctx = ctx;
}

void I8042SetKbdSink(I8042Device* d, void (*write_kbd)(void* ctx, uint8_t byte), void* ctx) {
  d->write_kbd = write_kbd;
  d->write_kbd_ctx = ctx;
}

void I8042SetTranslateSink(I8042Device* d, void (*set_translate)(void* ctx, int on), void* ctx) {
  d->set_translate = set_translate;
  d->translate_ctx = ctx;
}

void I8042KbdByte(I8042Device* d, uint8_t scancode) {
  if (DebugOn(kDbgMark)) DebugMark("kbd-byte", (int)scancode, 0);
  QueuePush(&d->st->kbd, scancode);
  I8042SyncIrq(d);
}

void I8042AuxByte(I8042Device* d, uint8_t byte) {
  // Command 0xa7 shuts the auxiliary interface down: the controller holds the
  // device's clock line low, so what the device sends while it is off never
  // reaches the output queue (PC/AT Technical Reference). QEMU keeps queueing
  // it and gates only the interrupt; v86 stops the stream, which is the same
  // idea.
  if (d->st->cmd_byte & kModeDisAux) return;
  if (DebugOn(kDbgMark)) DebugMark("aux-byte", (int)byte, 0);
  QueuePush(&d->st->aux, byte);
  I8042SyncIrq(d);
}
