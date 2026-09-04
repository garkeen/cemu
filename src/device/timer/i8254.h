#ifndef CEMU_DEVICE_TIMER_I8254_H
#define CEMU_DEVICE_TIMER_I8254_H

#include "bus/bus.h"

// Intel 8253/8254 programmable interval timer, three counter channels at
// 0x40-0x42 plus a mode control register at 0x43, as in the IBM PC/AT.
// Semantics follow the QEMU i8254 model (Fabrice Bellard, MIT; tiny386
// port): each channel has a 16-bit down-counter clocked at PIT_FREQ
// (1193182 Hz) derived from host wall-clock microseconds. Mode 2/3 deliver
// periodic edges (IRQ0 on channel 0 in real mode). Output drives a PIC IRQ
// via the set_irq callback like other devices.
//
// The host monotonic clock is read via HostTimerNow(); the machine poll loop
// calls PitPoll() every step (and more often while the CPU sleeps) so that
// the period elapses in wall time even during hlt.
typedef struct PitDevice {
  void (*set_irq)(void *ctx, int line, int level);
  void *irq_ctx;
  // channels[0] is IRQ0 (the periodic timer the BIOS programs).
  struct PitChannel *channels;
} PitDevice;

void PitInit(PitDevice *pit);
void PitRegister(Bus *io, PitDevice *pit, uint16_t base);
void PitSetIrqSink(PitDevice *pit, void (*set_irq)(void *, int, int),
                   void *ctx);
// Advances the counter state by the elapsed host time and emits any pending
// IRQ edges. Called from the machine poll loop.
void PitPoll(PitDevice *pit);

#endif
