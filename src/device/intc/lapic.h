#ifndef CEMU_DEVICE_INTC_LAPIC_H
#define CEMU_DEVICE_INTC_LAPIC_H

#include <stdint.h>

#include "bus/bus.h"

// The Local APIC (SDM vol.3 ch.11) at its default MMIO base 0xFEE00000: the
// register file, the interrupt bookkeeping around it (IRR/ISR/PPR), the
// CPU-facing INTR line, and the APIC timer. Delivery is what makes it more
// than storage: a vector handed to it by the I/O APIC (or any other source)
// lands in the IRR, the LAPIC asserts INTR while a request outranks both TPR
// and the highest in-service vector, and the processor's INTA cycle asks the
// LAPIC for the vector — the same division of labor QEMU models in
// hw/intc/apic.c (apic_deliver / apic_get_highest_priority_int).
//
// The timer counts down from the initial count at the bus clock divided by
// the TDCR divider (SDM vol.3 11.5.4). Like the PIT, cemu derives it from the
// host clock instead of counting steps, so the period holds in wall time even
// while the guest spins in hlt.
typedef struct LapicDevice {
  uint32_t reg[0x40];  // 4KB MMIO / 16-byte stride = 256 slots, 32-bit each
  uint8_t irr[32];     // request registers: one bit per vector
  uint8_t isr[32];     // in-service registers
  int64_t timer_base_us;  // host time the current count started from
  uint32_t timer_count;   // counts loaded at that instant (0 = timer idle)
  int irq_level;          // what this LAPIC last drove on the CPU's INTR line
  void (*set_irq)(void* ctx, int line, int level);
  void* irq_ctx;
  // The machine observes an EOI so that the I/O APIC can retire the pin whose
  // vector it was (82093AA remote-IRR).
  void (*eoi)(void* ctx, int vector);
  void* eoi_ctx;
} LapicDevice;

void LapicInit(LapicDevice* d);
void LapicRegister(Bus* bus, LapicDevice* d);
void LapicSetIrqSink(LapicDevice* d, void (*set_irq)(void* ctx, int line, int level), void* ctx);
void LapicSetEoiSink(LapicDevice* d, void (*eoi)(void* ctx, int vector), void* ctx);
// The APIC's own id (ID register bits 31:24) and its flat-model logical mask
// (LDR bits 31:24): what the machine's I/O APIC resolves a redirection entry's
// destination against.
int LapicId(const LapicDevice* d);
int LapicLogicalMask(const LapicDevice* d);
// Hands one vector to the LAPIC (an I/O APIC redirection entry, or any other
// machine source): it lands in the IRR and may raise INTR.
void LapicDeliver(LapicDevice* d, int vector);
// The INTA cycle: returns the vector to service and marks it in-service, or -1
// when this LAPIC has nothing for the processor (then the machine's other
// controller answers).
int LapicAcknowledge(LapicDevice* d);
// Timer countdown against the host clock; called from the machine poll loop.
void LapicPoll(LapicDevice* d);

extern const DeviceOps kLapicOps;

#endif
