#ifndef CEMU_DEVICE_MISC_PORT92_H
#define CEMU_DEVICE_MISC_PORT92_H

#include <stdint.h>

#include "bus/bus.h"

// System Control Port A (PC/AT Technical Reference; QEMU hw/i386/port92.c):
// one byte at port 0x92 whose bit 1 is the A20 gate and bit 0 asks for a CPU
// reset. SeaBIOS opens A20 through this port early in POST, so it has to exist
// before the chipset does.
//
// The gate starts open. Real ATs power up with A20 low, but every image cemu
// accepts — multiboot kernels, BIOS boot sectors — is handed flat memory
// above 1 MiB and never expects the alias; the guests that toggle the gate
// (xv6's bootasm, SeaBIOS, Linux) get real masking from here on.
typedef struct Port92Device {
  uint8_t outport;
  void (*set_a20)(void* ctx, int on);
  void* a20_ctx;
} Port92Device;

void Port92Init(Port92Device* d);
void Port92Register(Bus* io, Port92Device* d);
void Port92SetA20Sink(Port92Device* d, void (*set_a20)(void* ctx, int on), void* ctx);

#endif
