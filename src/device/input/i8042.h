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
// Keys arrive as scancode bytes — make codes, 0x80|make on a release, 0xe0
// before an extended key — pushed by the host window's keyboard
// (I8042KeyByte). A byte waiting at 0x60 sets output-buffer-full, and the
// controller holds its IRQ line up while a byte waits, the command byte
// enables the keyboard interrupt and the keyboard is clocked (PC/AT Technical
// Reference; QEMU pckbd's kbd_update_irq spells the same three conditions).
// That level is how the guest's keyboard driver learns a key is there; the
// guest drops it by reading the byte.
typedef struct I8042Device {
  void (*set_a20)(void* ctx, int on);
  void* a20_ctx;
  void (*set_irq)(void* ctx, int line, int level);
  void* irq_ctx;
  // Command 0xFE pulses the CPU's reset line: on a PC that is a machine reset,
  // and it is how SeaBIOS's i8042_reboot() asks for one (D18). The board
  // records the request and acts on it at the next instruction boundary.
  void (*request_reset)(void* ctx);
  void* reset_ctx;
  struct I8042State* st;  // private model state
} I8042Device;

void I8042Init(I8042Device* d);
void I8042Register(Bus* io, I8042Device* d);
void I8042SetA20Sink(I8042Device* d, void (*set_a20)(void* ctx, int on), void* ctx);
void I8042SetIrqSink(I8042Device* d, void (*set_irq)(void* ctx, int line, int level), void* ctx);
void I8042SetResetSink(I8042Device* d, void (*request_reset)(void* ctx), void* ctx);
// One byte from the keyboard device into the controller's output queue.
void I8042KeyByte(I8042Device* d, uint8_t scancode);

#endif
