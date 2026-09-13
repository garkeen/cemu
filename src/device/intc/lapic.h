#ifndef CEMU_DEVICE_INTC_LAPIC_H
#define CEMU_DEVICE_INTC_LAPIC_H

#include <stdint.h>

#include "bus/bus.h"

// The Local APIC (SDM vol.3 ch.11) at its default MMIO base 0xFEE00000.
// This build models the register file only: reads return the stored register
// value (ID reads as 0 = the single BSP), writes commit to the register
// bank. Interrupt DELIVERY (spurious/LVT/ICR-initiated) does not exist on
// this machine — the PIC is the only interrupt source — so delivery-relevant
// registers are storage. Register semantics grow with the stage-4 LAPIC work
// (kvm-unit-tests' harness reads the APIC ID and programs spurious/ICR).
typedef struct LapicDevice {
  uint32_t reg[0x40];  // 4KB MMIO / 16-byte stride = 256 slots, 32-bit each
} LapicDevice;

void LapicInit(LapicDevice* d);
void LapicRegister(Bus* bus, LapicDevice* d);

extern const DeviceOps kLapicOps;

#endif
