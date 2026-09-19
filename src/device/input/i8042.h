#ifndef CEMU_DEVICE_INPUT_I8042_H
#define CEMU_DEVICE_INPUT_I8042_H

#include "bus/bus.h"

// Intel 8042 keyboard controller as wired on the PC/AT: data port 0x60,
// status/command port 0x64. Every real-mode boot path polls it before it does
// anything else — xv6's bootasm spins on status bit 1 (IBF) clearing, and
// SeaBIOS's A20 code sends the 0xd1 ("write output port") command pair — so
// the status register must exist even before keys do.
//
// The output port carries the A20 gate in bit 1 (IBM PC/AT Technical
// Reference). The model drives the board's A20 line through a sink callback,
// the same shape as the IRQ sinks of the timers; the rest of the output port
// (reset, NMI, keyboard data) is stored but not wired.
//
// The key path itself (scancode queue, IRQ1) is the PS/2 slice of stage 4;
// until then a byte written to 0x60 outside a command sequence is recorded as
// the controller's "last data" exactly as the 8042 does when no device
// answers.
typedef struct I8042Device {
  void (*set_a20)(void* ctx, int on);
  void* a20_ctx;
  struct I8042State* st;  // private model state
} I8042Device;

void I8042Init(I8042Device* d);
void I8042Register(Bus* io, I8042Device* d);
void I8042SetA20Sink(I8042Device* d, void (*set_a20)(void* ctx, int on), void* ctx);

#endif
