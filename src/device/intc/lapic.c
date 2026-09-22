#include "device/intc/lapic.h"

#include "debug/debug.h"
#include "host/host.h"

// Register offsets (SDM vol.3 figure 11-18; one 32-bit register per 16 bytes).
enum {
  kLapicId = 0x20,
  kLapicVersion = 0x30,
  kLapicTpr = 0x80,
  kLapicPpr = 0xA0,
  kLapicEoi = 0xB0,
  kLapicLdr = 0xD0,
  kLapicDfr = 0xE0,
  kLapicSvr = 0xF0,
  kLapicEsr = 0x280,
  kLapicIcrLo = 0x300,
  kLapicLvtTimer = 0x320,
  kLapicTimerInitial = 0x380,  // TICR
  kLapicTimerCurrent = 0x390,  // TCCR, read-only
  kLapicTimerDivide = 0x3E0,   // TDCR
};
enum {
  kSvrEnable = 0x100,  // bit 8: APIC software enable (SDM 11.4.3)
};
// LVT timer bits (SDM vol.3 11.5.4): vector, mask, and the mode field.
enum {
  kLvtVector = 0xff,
  kLvtMasked = 0x10000,
  kLvtModeShift = 17,
  kLvtModePeriodic = 1,
};

static const uint64_t kLapicBase = 0xFEE00000ULL;
static const uint64_t kLapicSize = 0x1000ULL;
// The APIC bus clock the timer counts against (SDM vol.3 11.5.4: the counter
// runs at the bus clock divided by TDCR). A PC's bus clock is in the tens of
// MHz; QEMU's APIC model fixes it at 100MHz, so xv6's 10^7-count periodic
// timer comes out at 10Hz — a tick every 100ms, the interval xv6's own
// comment assumes.
static const uint64_t kLapicTimerFreq = 100000000ULL;

static int LowBit(uint8_t bits) {
  int n = 0;
  while (!(bits & 1)) {
    bits >>= 1;
    n++;
  }
  return n;
}

// The highest-priority request in a vector set: priority is the vector's top
// nibble, so the *lowest* vector number wins. Vectors below 16 are reserved
// and can never be delivered through the APIC (SDM vol.3 11.8.2).
static int HighestVector(const uint8_t* set) {
  for (int i = 2; i < 32; i++) {
    if (set[i]) return (i << 3) + LowBit(set[i]);
  }
  return -1;
}

static void VectorSet(uint8_t* set, int vector) { set[vector >> 3] |= (uint8_t)(1u << (vector & 7)); }

static void VectorClear(uint8_t* set, int vector) {
  set[vector >> 3] &= (uint8_t)~(1u << (vector & 7));
}

// The processor priority (SDM vol.3 11.8.3): the higher of TPR and the
// in-service set's top nibble.
static int ProcessorPriority(const LapicDevice* d) {
  int ppr = (int)((d->reg[kLapicTpr / 16] >> 4) & 0xf);
  int isr = HighestVector(d->isr);
  if (isr >= 0 && (isr >> 4) > ppr) ppr = isr >> 4;
  return ppr;
}

// A pending vector is deliverable when the APIC is enabled and it outranks the
// processor priority (SDM vol.3 11.8.2's priority arbitration).
static int Deliverable(const LapicDevice* d) {
  if (!(d->reg[kLapicSvr / 16] & kSvrEnable)) return 0;
  int vec = HighestVector(d->irr);
  return vec >= 0 && (vec >> 4) > ProcessorPriority(d);
}

// Drives the CPU's INTR line: asserted exactly while a request outranks the
// processor priority, so an EOI or a TPR write can drop it again.
static void UpdateIrq(LapicDevice* d) {
  int level = Deliverable(d);
  if (level == d->irq_level) return;
  d->irq_level = level;
  if (d->set_irq) d->set_irq(d->irq_ctx, 0, level);
}

void LapicDeliver(LapicDevice* d, int vector) {
  if (vector < 16 || vector > 255) return;
  // The APIC-side of the interrupt path is invisible from the board (it only
  // sees the INTR pin): whether a vector ever entered the APIC's IRR, whether
  // the APIC answered the INTA, and whether the handler's EOI retired it
  // (kDbgMark). A guest whose timer stops arriving is otherwise
  // indistinguishable from one that never got the interrupt.
  if (DebugOn(kDbgMark)) DebugMark("lapic-irr", vector, (int)d->reg[kLapicSvr / 16]);
  VectorSet(d->irr, vector);
  UpdateIrq(d);
}

int LapicAcknowledge(LapicDevice* d) {
  if (!(d->reg[kLapicSvr / 16] & kSvrEnable)) return -1;  // APIC off: the machine's PIC answers
  if (!Deliverable(d)) return -1;
  int vec = HighestVector(d->irr);
  VectorClear(d->irr, vec);
  VectorSet(d->isr, vec);  // in service until the handler's EOI
  if (DebugOn(kDbgMark)) DebugMark("lapic-ack", vec, (int)((d->reg[kLapicTpr / 16] >> 4) & 0xf));
  UpdateIrq(d);
  return vec;
}

// The TDCR divider table (SDM vol.3 table 11-9).
static uint32_t TimerDivisor(uint32_t tdcr) {
  static const uint32_t kDiv[8] = {2, 4, 8, 16, 32, 64, 128, 1};
  return kDiv[tdcr & 7];
}

// Counts the timer has burned since the count was loaded, at the bus clock
// divided by TDCR (SDM vol.3 11.5.4) — reconstructed from the host clock.
static uint32_t CountsElapsed(const LapicDevice* d, int64_t now_us) {
  int64_t us = now_us - d->timer_base_us;
  if (us <= 0) return 0;
  uint64_t counts = (uint64_t)us * (kLapicTimerFreq / TimerDivisor(d->reg[kLapicTimerDivide / 16])) /
                    1000000ULL;
  return counts > 0xffffffffULL ? 0xffffffffu : (uint32_t)counts;
}

static uint32_t TimerCurrent(const LapicDevice* d) {
  if (d->timer_count == 0) return 0;
  uint32_t burned = CountsElapsed(d, HostTimerNow());
  return burned >= d->timer_count ? 0 : d->timer_count - burned;
}

static void TimerArm(LapicDevice* d, uint32_t count) {
  d->timer_count = count;
  d->timer_base_us = HostTimerNow();
}

// One expiry: hand the LVT vector to the APIC itself and re-arm a periodic
// timer. The count is reloaded from the initial count, which is what makes the
// period the guest programmed hold.
static void TimerExpire(LapicDevice* d) {
  uint32_t lvt = d->reg[kLapicLvtTimer / 16];
  if (!(lvt & kLvtMasked) && (lvt & kLvtVector) >= 16) LapicDeliver(d, (int)(lvt & kLvtVector));
  uint32_t initial = d->reg[kLapicTimerInitial / 16];
  if (((lvt >> kLvtModeShift) & 3) == kLvtModePeriodic && initial != 0) {
    TimerArm(d, initial);
    return;
  }
  d->timer_count = 0;  // one-shot: the timer stops until reloaded
}

void LapicPoll(LapicDevice* d) {
  if (d->timer_count == 0) return;
  if (CountsElapsed(d, HostTimerNow()) < d->timer_count) return;
  TimerExpire(d);
}

int64_t LapicNextEventUs(LapicDevice* d) {
  if (d->timer_count == 0) return 0;
  uint64_t burned = CountsElapsed(d, HostTimerNow());
  if (burned >= d->timer_count) return 0;  // already due: LapicPoll has it
  uint64_t per_sec = kLapicTimerFreq / TimerDivisor(d->reg[kLapicTimerDivide / 16]);
  uint64_t left = d->timer_count - burned;
  return (int64_t)((left * 1000000 + per_sec - 1) / per_sec);  // round up: never 0 while pending
}

static uint64_t LapicRead(void* dev, uint64_t addr, int size) {
  (void)size;
  LapicDevice* d = (LapicDevice*)dev;
  uint64_t off = addr - kLapicBase;
  if (off & 0xf) return 0;  // reserved bytes inside a register
  switch (off) {
    case kLapicVersion:
      return 0x50014;  // version 0x14, 5 LVT entries (SDM vol.3 11.4.5)
    case kLapicId:
      return d->reg[off / 16] & 0xff000000u;  // the BSP is APIC 0, bits 31:24
    case kLapicPpr:
      return (uint32_t)(ProcessorPriority(d) << 4);
    case kLapicTimerCurrent:
      return TimerCurrent(d);
    case kLapicIcrLo: {
      uint32_t v = d->reg[off / 16];
      return v & ~(1u << 12);  // delivery status reads idle: IPIs complete at once
    }
    default:
      return d->reg[off / 16];
  }
}

static void LapicWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  LapicDevice* d = (LapicDevice*)dev;
  uint64_t off = addr - kLapicBase;
  if (off & 0xf) return;
  uint32_t v = (uint32_t)val;
  switch (off) {
    case kLapicEoi: {
      // End of interrupt: retire the highest-priority in-service vector
      // (SDM vol.3 11.8.4). Writing EOI with nothing in service is a no-op,
      // which is why the reset-time EOI in firmware's init is harmless.
      int vec = HighestVector(d->isr);
      if (vec < 0) return;
      if (DebugOn(kDbgMark)) DebugMark("lapic-eoi", vec, 0);
      VectorClear(d->isr, vec);
      if (d->eoi) d->eoi(d->eoi_ctx, vec);
      UpdateIrq(d);
      return;
    }
    case kLapicTpr:
      d->reg[off / 16] = v & 0xffu;  // TPR is one byte (SDM vol.3 11.4.11)
      UpdateIrq(d);
      return;
    case kLapicSvr:
      d->reg[off / 16] = v;
      UpdateIrq(d);  // clearing the enable bit drops INTR
      return;
    case kLapicPpr:
    case kLapicTimerCurrent:
      return;  // read-only
    case kLapicEsr:
      d->reg[off / 16] &= ~v;  // error status: write-1-to-clear
      return;
    case kLapicTimerInitial:
      d->reg[off / 16] = v;
      TimerArm(d, v);  // a write of 0 also stops the timer (SDM vol.3 11.5.4)
      return;
    case kLapicTimerDivide: {
      // The divider applies to the count in flight: fold the counts already
      // burned with the old divider into the loaded count before re-basing.
      uint32_t current = TimerCurrent(d);
      d->reg[off / 16] = v;
      if (current) TimerArm(d, current);
      return;
    }
    default:
      d->reg[off / 16] = v;
      return;
  }
}

const DeviceOps kLapicOps = {"lapic", LapicRead, LapicWrite};

void LapicInit(LapicDevice* d) {
  for (int i = 0; i < (int)(sizeof(d->reg) / sizeof(d->reg[0])); i++) d->reg[i] = 0;
  for (int i = 0; i < 32; i++) {
    d->irr[i] = 0;
    d->isr[i] = 0;
  }
  d->timer_count = 0;
  d->timer_base_us = 0;
  d->irq_level = 0;
  // Reset state (SDM vol.3 11.4.1): the APIC starts disabled with the
  // spurious vector at 0xFF, and TPR at 0 — the machine's firmware therefore
  // runs on the PIC until software enables the APIC.
  d->reg[kLapicSvr / 16] = 0xff;
  d->reg[kLapicTpr / 16] = 0;
  d->reg[kLapicDfr / 16] = 0xffffffffu;  // flat model (SDM vol.3 11.5.3)
  d->reg[kLapicLdr / 16] = 0;
}

void LapicRegister(Bus* bus, LapicDevice* d) { BusAddRegion(bus, kLapicBase, kLapicSize, &kLapicOps, d); }

void LapicSetIrqSink(LapicDevice* d, void (*set_irq)(void* ctx, int line, int level), void* ctx) {
  d->set_irq = set_irq;
  d->irq_ctx = ctx;
}

void LapicSetEoiSink(LapicDevice* d, void (*eoi)(void* ctx, int vector), void* ctx) {
  d->eoi = eoi;
  d->eoi_ctx = ctx;
}

// The APIC's own id (ID register bits 31:24), which a physical-mode I/O APIC
// entry addresses.
int LapicId(const LapicDevice* d) { return (int)(d->reg[kLapicId / 16] >> 24) & 0xf; }

// The flat-model logical destination: LDR bits 31:24 hold the mask (SDM vol.3
// 11.5.3), which is what an entry in logical mode addresses.
int LapicLogicalMask(const LapicDevice* d) { return (int)(d->reg[kLapicLdr / 16] >> 24) & 0xff; }
