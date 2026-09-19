#include "device/intc/ioapic.h"

// The register window (82093AA datasheet §3): an index register at 0x00 selects
// the register the data window at 0x10 answers.
enum {
  kRegSel = 0x00,
  kRegWin = 0x10,
  kRegId = 0x00,
  kRegVersion = 0x01,
  kRegArbitration = 0x02,
  kEntryBase = 0x10,
};
enum {
  // Bits 23:16 report version 0x11; the low byte reports the highest
  // redirection entry index (0x11 = 17) — the value xv6's ioapicinit reads
  // back as its "maxintr" and then writes entries for.
  kVersion = 0x00110011,
  // Delivery mode 000: hand the vector to the destination APIC, the only mode
  // a PC's ISA/PCI interrupts use (see 简化登记 D19 for the rest).
  kDeliveryFixed = 0,
};

static const uint64_t kIoapicBase = 0xFEC00000ULL;
static const uint64_t kIoapicSize = 0x1000ULL;

static void Service(IoapicDevice* d, int pin);

// The low dword of a redirection entry: vector, delivery mode, destination
// mode, polarity, trigger mode, mask, and the remote-IRR bit the chip itself
// maintains (82093AA §3.2.4). The delivery-status bit reads idle.
static uint32_t PinLow(const IoapicPin* p) {
  return (p->vector & 0xff) | ((p->delivery & 7) << 8) | ((p->dest_mode & 1) << 11) |
         ((p->polarity & 1) << 13) | ((p->remote_irr & 1) << 14) | ((p->trigger & 1) << 15) |
         ((p->mask & 1) << 16);
}

static void SetPinLow(IoapicPin* p, uint32_t v) {
  p->vector = v & 0xff;
  p->delivery = (v >> 8) & 7;
  p->dest_mode = (v >> 11) & 1;
  p->polarity = (v >> 13) & 1;
  p->trigger = (v >> 15) & 1;
  p->mask = (v >> 16) & 1;
  // remote_irr is the chip's, not the guest's: it survives the write.
}

// Hands one pin's request to the destination APIC. A level pin records the
// delivery in remote_irr, which is what keeps it from asking again until the
// handler's EOI clears it.
static void Service(IoapicDevice* d, int pin) {
  IoapicPin* p = &d->pins[pin];
  if (p->mask) return;
  if (p->delivery != kDeliveryFixed) return;
  if (p->trigger) p->remote_irr = 1;
  if (d->deliver) d->deliver(d->deliver_ctx, (int)p->dest, (int)p->dest_mode, (int)(p->vector & 0xff));
}

// A guest turning a pin on (clearing its mask) can find the line already
// asserted; a level request is then delivered now, exactly like the first
// assertion would have done.
static void Reassess(IoapicDevice* d, int pin) {
  IoapicPin* p = &d->pins[pin];
  if (!p->trigger || !p->level || p->remote_irr) return;
  Service(d, pin);
}

void IoapicSetPin(IoapicDevice* d, int pin, int level) {
  if (pin < 0 || pin >= kIoapicPins) return;
  IoapicPin* p = &d->pins[pin];
  // An active-low entry reads the wire inverted (PCI INTx are drawn that way).
  int asserted = p->polarity ? !level : level != 0;
  p->level = asserted;
  if (!asserted) return;
  if (p->trigger) {
    Reassess(d, pin);
    return;
  }
  Service(d, pin);  // edge: the transition itself is the request
}

void IoapicEoi(IoapicDevice* d, int vector) {
  for (int i = 0; i < kIoapicPins; i++) {
    IoapicPin* p = &d->pins[i];
    if (!p->remote_irr || (int)(p->vector & 0xff) != vector) continue;
    p->remote_irr = 0;
    // The wire is still asking: a level interrupt is retriggered after the EOI
    // (82093AA §3.2.4).
    if (p->level) Reassess(d, i);
  }
}

static uint64_t IoapicRead(void* dev, uint64_t addr, int size) {
  (void)size;
  IoapicDevice* d = (IoapicDevice*)dev;
  uint64_t off = addr - kIoapicBase;
  if (off == kRegSel) return d->index;
  if (off != kRegWin) return 0;
  switch (d->index) {
    case kRegId:
      return d->id << 24;
    case kRegVersion:
      return kVersion;
    case kRegArbitration:
      return 0;  // no arbitration bus on this machine
    default:
      break;
  }
  if (d->index < kEntryBase || d->index > kEntryBase + 2 * kIoapicPins - 1) return 0;
  const IoapicPin* p = &d->pins[(d->index - kEntryBase) >> 1];
  if ((d->index & 1) == 0) return PinLow(p);
  return p->dest << 24;
}

static void IoapicWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  IoapicDevice* d = (IoapicDevice*)dev;
  uint64_t off = addr - kIoapicBase;
  uint32_t v = (uint32_t)val;
  if (off == kRegSel) {
    d->index = v & 0xff;
    return;
  }
  if (off != kRegWin) return;
  switch (d->index) {
    case kRegId:
      d->id = (v >> 24) & 0xf;
      return;
    case kRegVersion:
    case kRegArbitration:
      return;  // read-only
    default:
      break;
  }
  if (d->index < kEntryBase || d->index > kEntryBase + 2 * kIoapicPins - 1) return;
  int pin = (int)((d->index - kEntryBase) >> 1);
  IoapicPin* p = &d->pins[pin];
  if ((d->index & 1) == 0)
    SetPinLow(p, v);
  else
    p->dest = (v >> 24) & 0xf;
  Reassess(d, pin);
}

const DeviceOps kIoapicOps = {"ioapic", IoapicRead, IoapicWrite};

void IoapicInit(IoapicDevice* d) {
  d->index = 0;
  d->id = 0;  // the id the firmware's MP table names
  for (int i = 0; i < kIoapicPins; i++) {
    IoapicPin* p = &d->pins[i];
    p->vector = 0;
    p->delivery = 0;
    p->dest_mode = 0;
    p->polarity = 0;
    p->remote_irr = 0;
    p->trigger = 0;
    // Reset state: every pin masked (82093AA §3.2.4), which is why firmware
    // can hand the machine over with the I/O APIC still silent.
    p->mask = 1;
    p->dest = 0;
    p->level = 0;
  }
}

void IoapicRegister(Bus* bus, IoapicDevice* d) { BusAddRegion(bus, kIoapicBase, kIoapicSize, &kIoapicOps, d); }

void IoapicSetDeliverSink(IoapicDevice* d,
                          void (*deliver)(void* ctx, int dest, int logical, int vector), void* ctx) {
  d->deliver = deliver;
  d->deliver_ctx = ctx;
}
