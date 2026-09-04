// Intel 8253/8254 programmable interval timer.
//
// Adapted from the QEMU i8254 model (Fabrice Bellard, MIT; tiny386 port) to
// the cemu device model. Three 16-bit down-counters clocked at PIT_FREQ
// (1.193182 MHz) derived from host wall-clock microseconds. Mode 2 and 3
// produce periodic edges; channel 0's edge raises PIC IRQ0 (the periodic
// timer interrupt that wakes hlt and drives real-mode scheduler ticks).
//
// The host monotonic clock is HostTimerNow() microseconds; the machine poll
// loop calls PitPoll() to let periods elapse in wall time even while the CPU
// sleeps (mirroring the CLINT mtime poll on the riscv side).

#include "device/timer/i8254.h"

#include <stdlib.h>
#include <string.h>

#include "host/host.h"

// PIT input clock frequency (PC/AT canonical 14.31818 MHz / 12).
#define PIT_FREQ 1193182

// LSB/MSB access state machine.
enum { kRwLsb = 1, kRwMsb = 2, kRwWord0 = 3, kRwWord1 = 4 };

typedef struct PitChannel {
  uint32_t count;          // programmed reload value (0 == 65536)
  uint16_t latched_count;
  uint8_t count_latched;   // 0 = none, else the rw_mode at latch time
  uint8_t status_latched;
  uint8_t status;
  uint8_t read_state;
  uint8_t write_state;
  uint8_t write_latch;
  uint8_t rw_mode;
  uint8_t mode;
  uint8_t bcd;              // not modeled
  uint8_t gate;
  uint64_t count_load_time; // HostTimerNow() us at last (re)load; 0 = unarmed
  uint64_t last_irq_count;  // tick count at last edge
  int armed;                // set after the first load; later loads anchor now
  int irq;                  // -1 if none (channels 1/2 in the PC)
} PitChannel;

struct PitDevicePrivate {
  PitChannel channels[3];
};

// Ticks elapsed since (re)load, at PIT_FREQ from host microseconds. The
// anchor is lazily armed on the first Poll after a (re)load: machine
// creation and image loading take wall time, and the counter must not run
// before the CPU does (QEMU runs the PIT on the virtual clock, which only
// advances while the guest executes).
static uint64_t PitElapsed(PitChannel *s) {
  uint64_t now = HostTimerNow();
  if (s->count_load_time == 0) {
    s->count_load_time = now;
    return 0;
  }
  return (now - s->count_load_time) * PIT_FREQ / 1000000;
}

static int PitGetCount(PitChannel *s) {
  uint64_t d = PitElapsed(s);
  switch (s->mode) {
    case 0: case 1: case 4: case 5:
      return (s->count - d) & 0xffff;
    case 3:
      // square wave: odd counts behave slightly differently (QEMU comment).
      return s->count - ((2 * d) % s->count);
    default:
      return s->count - (d % s->count);
  }
}

static int PitGetOut(PitChannel *s) {
  uint64_t d = PitElapsed(s);
  uint64_t count = s->count ? s->count : 0x10000;
  switch (s->mode) {
    default:
    case 0: return d >= count;
    case 1: return d < count;
    case 2: return (d % count) == 0 && d != 0;
    case 3: return (d % count) < ((count + 1) >> 1);
    case 4: case 5: return d == count;
  }
}

static void PitLoadCount(PitChannel *s, int val) {
  if (val == 0) val = 0x10000;
  // 0 = unarmed: the anchor is armed on the first Poll after this load so
  // that machine construction / image loading wall time is not counted as
  // elapsed PIT ticks (the counter only runs while the guest does).
  s->count_load_time = s->armed ? HostTimerNow() : 0;
  s->last_irq_count = 0;
  s->count = (uint32_t)val;
  s->armed = 1;
}

static void PitLatchCount(PitChannel *s) {
  if (!s->count_latched) {
    s->latched_count = (uint16_t)PitGetCount(s);
    s->count_latched = s->rw_mode;
  }
}

static void PitIoWrite(PitDevice *pit, uint16_t addr, uint8_t val) {
  if (addr == 3) {
    // mode/control register
    int channel = val >> 6;
    if (channel == 3) {
      // read-back command
      for (channel = 0; channel < 3; channel++) {
        PitChannel *s = &pit->channels[channel];
        if (val & (2 << channel)) {
          if (!(val & 0x20)) PitLatchCount(s);
          if (!(val & 0x10) && !s->status_latched) {
            s->status = (PitGetOut(s) << 7) | (s->rw_mode << 4)
                        | (s->mode << 1) | s->bcd;
            s->status_latched = 1;
          }
        }
      }
    } else {
      PitChannel *s = &pit->channels[channel];
      int access = (val >> 4) & 3;
      if (access == 0) {
        PitLatchCount(s);
      } else {
        s->rw_mode = (uint8_t)access;
        s->read_state = (uint8_t)access;
        s->write_state = (uint8_t)access;
        s->mode = (val >> 1) & 7;
        s->bcd = val & 1;
      }
    }
    return;
  }
  PitChannel *s = &pit->channels[addr];
  switch (s->write_state) {
    case kRwLsb:
      PitLoadCount(s, val);
      break;
    case kRwMsb:
      PitLoadCount(s, val << 8);
      break;
    case kRwWord0:
      s->write_latch = val;
      s->write_state = kRwWord1;
      break;
    case kRwWord1:
      PitLoadCount(s, s->write_latch | (val << 8));
      s->write_state = kRwWord0;
      break;
  }
}

static uint8_t PitIoRead(PitDevice *pit, uint16_t addr) {
  PitChannel *s = &pit->channels[addr];
  if (s->status_latched) {
    s->status_latched = 0;
    return s->status;
  }
  if (s->count_latched) {
    switch (s->count_latched) {
      case kRwLsb:
        s->count_latched = 0;
        return s->latched_count & 0xff;
      case kRwMsb:
        s->count_latched = 0;
        return s->latched_count >> 8;
      case kRwWord0:
        s->count_latched = kRwMsb;
        return s->latched_count & 0xff;
      default:
        return 0;  // unreachable
    }
  }
  int count = PitGetCount(s);
  switch (s->read_state) {
    case kRwLsb: return count & 0xff;
    case kRwMsb: return (count >> 8) & 0xff;
    case kRwWord0:
      s->read_state = kRwWord1;
      return count & 0xff;
    case kRwWord1:
      s->read_state = kRwWord0;
      return (count >> 8) & 0xff;
    default:
      return 0;
  }
}

static void PitReset(PitDevice *pit) {
  for (int i = 0; i < 3; i++) {
    PitChannel *s = &pit->channels[i];
    s->mode = 3;
    s->gate = (i != 2);
    s->irq = -1;
    PitLoadCount(s, 0);
  }
}

void PitPoll(PitDevice *pit) {
  // Only channel 0 carries an IRQ in the standard PC wiring.
  PitChannel *s = &pit->channels[0];
  if (s->irq == -1) return;
  uint64_t d = PitElapsed(s);
  switch (s->mode) {
    case 2:
    case 3: {
      // QEMU: assert an edge whenever the period boundary is crossed since
      // the last delivery. last_irq_count is in the same tick domain as d.
      // Detect wrap of (d - last_irq_count) past 0x80000000 of uint32 ticks.
      uint32_t delta = (uint32_t)(d - s->last_irq_count);
      // QEMU uses (s->count - d + last_irq_count) wrap test; we mirror it:
      if ((int32_t)(s->last_irq_count + s->count - (uint32_t)d) < 0) {
        if (pit->set_irq) {
          pit->set_irq(pit->irq_ctx, s->irq, 1);
          pit->set_irq(pit->irq_ctx, s->irq, 0);
        }
        s->last_irq_count += s->count;
        if (HostTimerNow() - s->count_load_time > (1ULL << 31))
          PitLoadCount(s, s->count);
      }
      (void)delta;
      break;
    }
    default:
      break;
  }
}

static uint64_t PitRead(void *dev, uint64_t addr, int size) {
  (void)size;
  return PitIoRead((PitDevice *)dev, (uint16_t)(addr & 3));
}
static void PitWrite(void *dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  PitIoWrite((PitDevice *)dev, (uint16_t)(addr & 3), (uint8_t)val);
}

static const DeviceOps kPitOps = {"8254", PitRead, PitWrite};

void PitInit(PitDevice *pit) {
  pit->channels =
      (PitChannel *)calloc(3, sizeof(PitChannel));
  PitReset(pit);
  pit->channels[0].irq = 0;  // IRQ0
  pit->set_irq = NULL;
  pit->irq_ctx = NULL;
}

void PitRegister(Bus *io, PitDevice *pit, uint16_t base) {
  BusAddRegion(io, base, 4, &kPitOps, pit);
}

void PitSetIrqSink(PitDevice *pit, void (*set_irq)(void *, int, int),
                   void *ctx) {
  pit->set_irq = set_irq;
  pit->irq_ctx = ctx;
}
