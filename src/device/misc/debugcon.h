#ifndef CEMU_DEVICE_MISC_DEBUGCON_H
#define CEMU_DEVICE_MISC_DEBUGCON_H

#include <stdint.h>

#include "bus/bus.h"

// QEMU isa-debugcon (hw/char/debugcon.c): a byte written to the port is
// emitted on the host's debug console, and a read answers the probe value
// 0xe9 with which firmware decides whether the port exists (seabios
// paravirt.h QEMU_DEBUGCON_READBACK, src/hw/serialio.c qemu_debug_preinit).
// This is the PC board's POST log channel — the same one the seabios build
// was validated against in QEMU (-device isa-debugcon,iobase=0x402).
//
// Stateless: writes print to stdout (the same stream the UART's TX uses),
// reads answer the probe value.
extern const DeviceOps kDebugConOps;

void DebugConRegister(Bus* io, uint16_t port);

#endif
