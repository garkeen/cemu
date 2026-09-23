#ifndef CEMU_DEVICE_INTC_IOAPIC_H
#define CEMU_DEVICE_INTC_IOAPIC_H

#include <stdint.h>

#include "bus/bus.h"

// Intel 82093AA I/O APIC at its PC address 0xFEC00000: the register window
// (IOREGSEL at 0x00, IOWIN at 0x10) and 24 redirection entries, each telling
// the chip where one input pin's interrupt goes. The entries are what a real
// operating system programs instead of the 8259: xv6 masks every pin with
// (word 93... no) — with the disabled entry pattern, then enables the ones it
// the 8259 pair: an operating system programs the redirection entries instead
// (xv6 disables every pin, then enables the ones it wants — ioapicinit /
// ioapicenable), while firmware keeps running on the PIC.
// The pins are the machine's ISA IRQ lines, wired to the same wires as the
// 8259 pair; which controller delivers is the guest's choice, exactly as on a
// PC/AT. Delivery is fixed-mode only (see 简化登记 D19): a redirection entry's
// vector goes to the destination APIC, whose ID (physical mode) or logical
// mask (flat model) the machine resolves.
enum { kIoapicPins = 24 };

typedef struct IoapicPin {
  uint32_t vector;      // bits 7:0
  uint32_t delivery;    // bits 10:8: delivery mode
  uint32_t dest_mode;   // bit 11: 1 = logical destination
  uint32_t polarity;    // bit 13: 1 = active low (the PCI INTx convention)
  uint32_t remote_irr;  // bit 14: set while a delivered level request is in service
  uint32_t trigger;     // bit 15: 1 = level triggered
  uint32_t mask;        // bit 16
  uint32_t dest;        // bits 31:24: destination APIC ID / logical mask
  int level;            // the input wire's asserted state (polarity applied)
} IoapicPin;

typedef struct IoapicDevice {
  uint32_t index;  // IOREGSEL: which register the data window addresses
  uint32_t id;     // bits 27:24 of the ID register
  IoapicPin pins[kIoapicPins];
  // Where a delivered entry goes: the destination APIC's id (physical mode) or
  // its logical mask (logical mode), the vector, and the entry's trigger mode
  // (0 edge, 1 level) — the APIC records the latter in its TMR so the handler
  // can tell the two apart (SDM vol.3 11.5.8).
  void (*deliver)(void* ctx, int dest, int logical, int vector, int trigger);
  void* deliver_ctx;
} IoapicDevice;

void IoapicInit(IoapicDevice* d);
void IoapicRegister(Bus* bus, IoapicDevice* d);
void IoapicSetDeliverSink(IoapicDevice* d,
                          void (*deliver)(void* ctx, int dest, int logical, int vector, int trigger),
                          void* ctx);
// An input pin changes level — the machine's device IRQ wires.
void IoapicSetPin(IoapicDevice* d, int pin, int level);
// An end-of-interrupt for a vector: retires the remote-IRR bit of the level
// entry that delivered it and re-requests if the pin is still asserted
// (82093AA datasheet "End of Interrupt").
void IoapicEoi(IoapicDevice* d, int vector);

extern const DeviceOps kIoapicOps;

#endif
