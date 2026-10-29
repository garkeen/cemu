// Intel 8253/8254 programmable interval timer.
//
// Adapted from the QEMU i8254 model (Fabrice Bellard, MIT; tiny386 port) to
// the cemu device model. Three 16-bit down-counters clocked at PIT_FREQ
// (1.193182 MHz) derived from the microseconds HostTimerNow() reports. Mode 2
// and 3 produce periodic edges; channel 0's edge raises PIC IRQ0 (the periodic
// timer interrupt that wakes hlt and drives real-mode scheduler ticks).
//
// HostTimerNow() is the emulated clock (host/host.h): host wall time by
// default, a declared per-instruction rate under -icount. Either way the machine
// poll loop calls PitPoll() once per step, so a period elapses even while the
// CPU sleeps — and it lets exactly one edge per period boundary through since
// the last delivery, so a step that covers several periods does not deliver a
// burst (QEMU hw/timer/i8254.c pit_irq_timer, same contract).

#include "device/timer/i8254.h"

#include <stdlib.h>
#include <string.h>

#include "host/host.h"

// PIT input clock frequency (PC/AT canonical 14.31818 MHz / 12).
#define PIT_FREQ 1193182

// LSB/MSB access state machine.
enum { kRwLsb = 1, kRwMsb = 2, kRwWord0 = 3, kRwWord1 = 4 };

typedef struct PitChannel {
  uint32_t count;  // programmed reload value (0 == 65536)
  uint16_t latched_count;
  uint8_t count_latched;  // 0 = none, else the rw_mode at latch time
  uint8_t status_latched;
  uint8_t status;
  uint8_t read_state;
  uint8_t write_state;
  uint8_t write_latch;
  uint8_t rw_mode;
  uint8_t mode;
  uint8_t bcd;  // not modeled
  uint8_t gate;
  uint64_t count_load_time;  // HostTimerNow() us at last (re)load; 0 = unarmed
  uint64_t last_irq_count;   // tick count at last edge
  int out_level;             // last driven OUT level; the PIC sees the rising edges
  int armed;                 // set after the first load; later loads anchor now
  int irq;                   // -1 if none (channels 1/2 in the PC)
} PitChannel;

struct PitDevicePrivate {
  PitChannel channels[3];
};

// Ticks elapsed since (re)load, at PIT_FREQ from host microseconds. The
// anchor is lazily armed on the first Poll after a (re)load: machine
// creation and image loading take wall time, and the counter must not run
// before the CPU does (QEMU runs the PIT on the virtual clock, which only
// advances while the guest executes).
static uint64_t PitElapsed(PitChannel* s) {
  uint64_t now = HostTimerNow();
  if (s->count_load_time == 0) {
    s->count_load_time = now;
    return 0;
  }
  return (now - s->count_load_time) * PIT_FREQ / 1000000;
}

static int PitGetCount(PitChannel* s) {
  uint64_t d = PitElapsed(s);
  switch (s->mode) {
    case 0:
    case 1:
    case 4:
    case 5:
      return (s->count - d) & 0xffff;
    case 3:
      // square wave: odd counts behave slightly differently (QEMU comment).
      return s->count - ((2 * d) % s->count);
    default:
      return s->count - (d % s->count);
  }
}

static int PitGetOut(PitChannel* s) {
  uint64_t d = PitElapsed(s);
  uint64_t count = s->count ? s->count : 0x10000;
  switch (s->mode) {
    default:
    case 0:
      return d >= count;
    case 1:
      return d < count;
    case 2:
      return (d % count) == 0 && d != 0;
    case 3:
      return (d % count) < ((count + 1) >> 1);
    case 4:
    case 5:
      return d == count;
  }
}

// Drive the IRQ line to the current OUT level, both edges: QEMU's
// pit_irq_timer_update does exactly this with qemu_set_irq(pit_get_out()).
// The line must be a level, not a one-call pulse: an edge-triggered 8259
// latches the rise in its IRR on the call, while a controller a guest has put
// in level-sensitive mode (ICW1.LTIM or the ELCR) follows the line itself, so
// a raise and a lower inside the same call leave neither a latched request nor
// an asserted level for the CPU's INTR sampling to see. Mode 3's OUT is high
// as soon as the count loads, so the first tick lands at the load, not one
// period later -- and the following rising edges stay at full-count boundaries.
static void PitDriveOut(PitDevice* pit, PitChannel* s) {
  int o = PitGetOut(s);
  if (o == s->out_level) return;
  s->out_level = o;
  if (s->irq != -1 && pit->set_irq) pit->set_irq(pit->irq_ctx, s->irq, o);
  if (o && s->mode == 3) {
    uint64_t d = PitElapsed(s);
    s->last_irq_count = (d / s->count) * s->count;
  }
}

static void PitLoadCount(PitDevice* pit, PitChannel* s, int val) {
  if (val == 0) val = 0x10000;
  // 0 = unarmed: the anchor is armed on the first Poll after this load so
  // that machine construction / image loading wall time is not counted as
  // elapsed PIT ticks (the counter only runs while the guest does).
  s->count_load_time = s->armed ? HostTimerNow() : 0;
  s->last_irq_count = 0;
  s->count = (uint32_t)val;
  s->armed = 1;
  if (s->mode == 3) PitDriveOut(pit, s);
}

static void PitLatchCount(PitChannel* s) {
  if (!s->count_latched) {
    s->latched_count = (uint16_t)PitGetCount(s);
    s->count_latched = s->rw_mode;
  }
}

static void PitIoWrite(PitDevice* pit, uint16_t addr, uint8_t val) {
  if (addr == 3) {
    // mode/control register
    int channel = val >> 6;
    if (channel == 3) {
      // read-back command
      for (channel = 0; channel < 3; channel++) {
        PitChannel* s = &pit->channels[channel];
        if (val & (2 << channel)) {
          if (!(val & 0x20)) PitLatchCount(s);
          if (!(val & 0x10) && !s->status_latched) {
            s->status = (PitGetOut(s) << 7) | (s->rw_mode << 4) | (s->mode << 1) | s->bcd;
            s->status_latched = 1;
          }
        }
      }
    } else {
      PitChannel* s = &pit->channels[channel];
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
  PitChannel* s = &pit->channels[addr];
  switch (s->write_state) {
    case kRwLsb:
      PitLoadCount(pit, s, val);
      break;
    case kRwMsb:
      PitLoadCount(pit, s, val << 8);
      break;
    case kRwWord0:
      s->write_latch = val;
      s->write_state = kRwWord1;
      break;
    case kRwWord1:
      PitLoadCount(pit, s, s->write_latch | (val << 8));
      s->write_state = kRwWord0;
      break;
  }
}

static uint8_t PitIoRead(PitDevice* pit, uint16_t addr) {
  PitChannel* s = &pit->channels[addr];
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
    case kRwLsb:
      return count & 0xff;
    case kRwMsb:
      return (count >> 8) & 0xff;
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

static void PitReset(PitDevice* pit) {
  for (int i = 0; i < 3; i++) {
    PitChannel* s = &pit->channels[i];
    s->mode = 3;
    s->gate = (i != 2);
    s->irq = -1;
    s->out_level = 0;
    PitLoadCount(pit, s, 0);
  }
}

void PitPoll(PitDevice* pit) {
  // Only channel 0 carries an IRQ in the standard PC wiring.
  PitChannel* s = &pit->channels[0];
  if (s->irq == -1) return;
  if (s->mode == 3) {
    // Square wave: the line level itself carries the edges (high right after
    // the load, low after the half period, high again at each full count).
    PitDriveOut(pit, s);
    return;
  }
  uint64_t d = PitElapsed(s);
  switch (s->mode) {
    case 2: {
      // Rate generator: OUT is high except for one input clock at the terminal
      // count, too narrow to sample, so the boundary crossing is the edge.
      // last_irq_count is in the same tick domain as d, and the tick is due at
      // d == last_irq_count + count: that is the transition QEMU's
      // pit_get_next_transition_time names, so a run warp that lands exactly on
      // the deadline fires here instead of needing one more microsecond.
      if ((int32_t)(s->last_irq_count + s->count - (uint32_t)d) <= 0) {
        // The tick itself: OUT dips and rises (8254 mode 2). Both levels are
        // driven so an edge-triggered 8259 latches the rise while a
        // level-sensitive one sees the asserted line; the level stays up until
        // the next poll (below) so the CPU samples it at an instruction
        // boundary -- QEMU's qemu_set_irq(1) likewise survives until the OUT
        // transition that lowers it.
        s->out_level = 1;
        if (pit->set_irq) {
          pit->set_irq(pit->irq_ctx, s->irq, 0);
          pit->set_irq(pit->irq_ctx, s->irq, 1);
        }
        s->last_irq_count += s->count;
        if (HostTimerNow() - s->count_load_time > (1ULL << 31)) PitLoadCount(pit, s, s->count);
      } else if (s->out_level) {
        // A poll later OUT is low again: drop the line so the next terminal
        // count is a fresh rising edge (and a level-sensitive controller does
        // not hold a request the guest has already acknowledged).
        s->out_level = 0;
        if (pit->set_irq) pit->set_irq(pit->irq_ctx, s->irq, 0);
      }
      break;
    }
    default:
      break;
  }
}

int64_t PitNextEventUs(PitDevice* pit) {
  PitChannel* s = &pit->channels[0];
  if (s->irq == -1) return 0;
  if (s->mode != 2 && s->mode != 3) return 0;  // only these deliver edges
  if (s->count_load_time == 0) return 0;       // never loaded: nothing to wait for
  // PitPoll fires when d reaches last_irq_count + count (same tick domain as
  // PitElapsed, so the two agree on when the edge is).
  uint64_t d = PitElapsed(s);
  uint64_t next = s->last_irq_count + s->count;
  if (next <= d) return 0;  // already due: PitPoll has it
  return (int64_t)((next - d) * 1000000 / PIT_FREQ);
}

static uint64_t PitRead(void* dev, uint64_t addr, int size) {
  (void)size;
  return PitIoRead((PitDevice*)dev, (uint16_t)(addr & 3));
}
static void PitWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  PitIoWrite((PitDevice*)dev, (uint16_t)(addr & 3), (uint8_t)val);
}

static const DeviceOps kPitOps = {"8254", PitRead, PitWrite};

void PitInit(PitDevice* pit) {
  if (!pit->channels) {
    pit->channels = (PitChannel*)calloc(3, sizeof(PitChannel));
    if (!pit->channels) return;
  }
  // Re-initialising a live timer is the machine's reset path (D18): the three
  // channels go back to power-on without a second allocation.
  PitReset(pit);
  pit->channels[0].irq = 0;  // IRQ0
  pit->set_irq = NULL;
  pit->irq_ctx = NULL;
}

void PitRegister(Bus* io, PitDevice* pit, uint16_t base) {
  BusAddRegion(io, base, 4, &kPitOps, pit);
}

void PitSetIrqSink(PitDevice* pit, void (*set_irq)(void*, int, int), void* ctx) {
  pit->set_irq = set_irq;
  pit->irq_ctx = ctx;
}
