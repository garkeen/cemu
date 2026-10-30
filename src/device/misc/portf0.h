#ifndef CEMU_DEVICE_MISC_PORTF0_H
#define CEMU_DEVICE_MISC_PORTF0_H

#include <stdint.h>

#include "bus/bus.h"

// Port 0xF0, the chipset's IGNNE# latch for "MS-DOS compatibility mode" x87
// exception reporting (QEMU hw/i386/pc.c ioportF0_io, registered by the PC
// machine rather than by the chipset itself).
//
// Any write asserts IGNNE#. The processor answers by ignoring a pending
// unmasked x87 exception — and, on this machine, by dropping the FERR# output
// the board turns into IRQ13 — which is what lets an MS-DOS extender keep going
// after an FPU error its own handler has already seen.
//
// The data byte is ignored, as in QEMU ("if (tcg_enabled()) cpu_set_ignne()"):
// the 486 hardware distinguishes assert from deassert by the written bit, and
// the reference does not model that distinction.
//
// Reads answer all ones, the same as an unclaimed port on this machine and the
// value QEMU's ioportF0_read returns.
typedef struct PortF0Device {
  void (*set_ignne)(void* ctx, int on);
  void* ctx;
} PortF0Device;

void PortF0Init(PortF0Device* d);
void PortF0Register(Bus* io, PortF0Device* d);
void PortF0SetIgnneSink(PortF0Device* d, void (*set_ignne)(void* ctx, int on), void* ctx);

#endif
