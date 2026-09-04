#ifndef CEMU_DEVICE_CHAR_UART16550_H
#define CEMU_DEVICE_CHAR_UART16550_H

#include <stdint.h>
#include "bus/bus.h"

// 16550-style UART, transmit side only (console out). Registers are 1 byte
// wide at the 8 offsets of the device window, which covers both the PC COM1
// I/O-port form and the QEMU virt MMIO form. There is no receiver (no input
// device is wired to it yet) and LSR therefore never reports data ready.
// The interrupt line follows dearchap-tinyemu x86_machine.c
// serial_update_irq: only THRI can raise it (no receiver), and it is a level
// output asserted while (IER.THRI && THRE). Boards that have no interrupt
// controller for it simply leave the sink unset.
typedef struct Uart16550 {
  uint8_t ier;
  uint8_t iir;
  uint8_t lcr;
  uint8_t mcr;
  uint8_t msr;
  uint8_t scr;
  uint8_t fcr;
  uint8_t lsr;     // read-only status, THRE/TEMT stay set (immediate transmit)
  uint16_t divider;  // divisor latch, visible at offsets 0/1 while DLAB=1
  void (*set_irq)(void *ctx, int src, int level);
  void *irq_ctx;
} Uart16550;

void Uart16550Init(Uart16550 *u);
uint8_t Uart16550Read(Uart16550 *u, uint64_t off);
void Uart16550Write(Uart16550 *u, uint64_t off, uint8_t val);
void Uart16550SetIrqSink(Uart16550 *u, void (*set_irq)(void *, int, int),
                         void *ctx);

extern const DeviceOps kUart16550Ops;

#endif
