#ifndef CEMU_DEVICE_VIRTIO_VIRTIO_CONSOLE_H
#define CEMU_DEVICE_VIRTIO_VIRTIO_CONSOLE_H

#include <stdint.h>

#include "bus/bus.h"
#include "device/virtio/virtio.h"

// VirtIO console, port 0 (spec 5.3): queue 0 is the receive queue the host
// fills (guest-writable buffers the driver posts), queue 1 carries guest
// output. Guest bytes go to the host console immediately; host bytes are
// buffered until the driver has a receive buffer posted, then delivered with
// a used-ring interrupt. Reference: dearchap-tinyemu virtio.c
// virtio_console_*.

enum { kVirtioConsoleRxBuf = 256 };

typedef struct VirtioConsole {
  VirtioDevice vdev;
  uint8_t rx[kVirtioConsoleRxBuf];  // host -> guest staging ring
  int rx_head, rx_count;
  int64_t last_tx_poll_us;  // Tx drain throttle (see VirtioConsolePoll)
} VirtioConsole;

VirtioConsole* VirtioConsoleCreate(Bus* bus, uint64_t base, uint64_t size, int irq_src,
                                   void (*set_irq)(void*, int, int), void* irq_ctx);
// Host input: one byte into the staging ring; delivery happens on Poll once
// the driver posts receive buffers (Linux's hvc does this from its ISR).
void VirtioConsoleReceive(VirtioConsole* con, int ch);
// Drain the staging ring into posted buffers; call from the board poll.
void VirtioConsolePoll(VirtioConsole* con);
void VirtioConsoleDestroy(VirtioConsole* con);

#endif
