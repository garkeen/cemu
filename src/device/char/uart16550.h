#ifndef CEMU_DEVICE_CHAR_UART16550_H
#define CEMU_DEVICE_CHAR_UART16550_H

#include <stdint.h>

#include "bus/bus.h"

// 16550-style UART: a full-duplex console. Registers are 1 byte wide at the 8
// offsets of the device window, which covers both the PC COM1 I/O-port form and
// the QEMU virt MMIO form.
//
// Transmit is immediate — a THR write leaves THRE/TEMT set, so a polled guest
// and an interrupt-driven one both make progress — while the receiver is the
// real chip: a 16-byte FIFO with the trigger levels, the character timeout, the
// line-status error bits and the interrupt-priority order of the 16550D
// datasheet. A guest that configures the port the way Linux's 8250 driver does
// (FIFO enable, trigger level, IIR priority, loopback autoprobe) sees the same
// behaviour it would from the chip.
//
// The interrupt line is a level output asserted while any *enabled* source is
// pending (16550D "Interrupt Identification"; QEMU hw/char/serial.c
// serial_update_irq raises/lowers it the same way). Boards with no interrupt
// controller for it simply leave the sink unset.
typedef struct Uart16550 {
  uint8_t ier;  // interrupt enable: ERBFI/ETBEI/ELSI/EDSSI
  uint8_t iir;  // interrupt identification (read-only)
  uint8_t lcr;  // line control: word length, stop, parity, break, DLAB
  uint8_t mcr;  // modem control (bit 4 = loopback)
  uint8_t msr;  // modem status (read-only; the delta bits clear on read)
  uint8_t scr;  // scratch
  uint8_t fcr;  // FIFO control: enable, clear rx/tx, trigger level
  uint8_t lsr;  // line status: DR/OE/PE/FE/BI/THRE/TEMT
  uint16_t divider;  // divisor latch, visible at offsets 0/1 while DLAB=1

  // Receiver: the 16550's 16-byte FIFO plus the timeout interrupt that flushes
  // a burst which stops short of the trigger level. Depth, trigger levels and
  // the 4-character-time timeout are the chip's, not ours.
  uint8_t rx_fifo[16];
  int rx_head;
  int rx_tail;
  int rx_count;
  int rx_trigger;         // 1/4/8/14, from FCR bits 7:6
  int rx_timeout_armed;   // a character-time timer is running
  int64_t rx_timeout_at;  // host microseconds when it expires
  int rx_timeout_ip;      // the timeout interrupt is pending (IIR 0b110)
  int thr_ip;             // THR-empty interrupt pending (one-shot, 16550D)

  void (*set_irq)(void* ctx, int src, int level);
  void* irq_ctx;
} Uart16550;

void Uart16550Init(Uart16550* u);
uint8_t Uart16550Read(Uart16550* u, uint64_t off);
void Uart16550Write(Uart16550* u, uint64_t off, uint8_t val);
// Host input -> the receiver, one byte at a time, exactly as SIN would deliver
// it (dropped while the port loops back: loopback disconnects SIN).
void Uart16550Receive(Uart16550* u, int ch);
// Character-timeout expiry, from the board's poll. The timeout is the one
// receiver event with no register access to hang off: a byte arriving is the
// guest's business, but the chip decides on its own that a partial FIFO has
// waited long enough, so a halted guest must still be woken for it.
void Uart16550Poll(Uart16550* u);
void Uart16550SetIrqSink(Uart16550* u, void (*set_irq)(void*, int, int), void* ctx);

extern const DeviceOps kUart16550Ops;

#endif
