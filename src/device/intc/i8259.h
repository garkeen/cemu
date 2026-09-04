#ifndef CEMU_DEVICE_INTC_I8259_H
#define CEMU_DEVICE_INTC_I8259_H

#include "bus/bus.h"

// Intel 8259A programmable interrupt controller, two-chip cascade
// (master at 0x20-0x21, slave at 0xA0-0xA1) as wired in every IBM PC/AT.
// Semantics follow the QEMU i8259 model (Fabrice Bellard, MIT): edge-
// triggered IRR, fixed priority by rotating `priority_add`, auto-EOI and
// special fully nested mode, ICW1..4 init sequence. IRQs 0..7 feed the
// master, 8..15 the slave (whose INT line drives master IRQ2).
//
// The device speaks the level-set callback `set_irq` like the CLINT does
// on the riscv side; the machine wires it to the CPU's RaiseIrq.
typedef struct PicDevice {
  // 0 = master, 1 = slave
  struct PicState* pics;
  void (*set_irq)(void* ctx, int line, int level);
  void* irq_ctx;
} PicDevice;

void PicInit(PicDevice* pic);
void PicRegister(Bus* io, PicDevice* pic, uint16_t master_base, uint16_t slave_base);
void PicSetIrqSink(PicDevice* pic, void (*set_irq)(void*, int, int), void* ctx);
// Called by the CPU's interrupt-acknowledge cycle: returns the vector number
// (irq_base + irq) of the highest-priority pending unmasked IRQ, or a
// spurious vector if none. Performs the intack (clears IRR/sets ISR, EOI per
// auto-eoi mode). After the call the CPU reads the vector to dispatch.
// Device-facing IRQ input: raises/lowers one of the 16 cascade IRQ lines
// (0..15; 8..15 land on the slave whose INT feeds master IRQ2). Edge
// triggered like the real 8259A.
void PicSetIrq(PicDevice* pic, int irq, int level);
int PicAcknowledge(PicDevice* pic);
// Returns 1 if any unmasked IRQ is pending and deliverable (i.e. the CPU
// should observe its INTR line asserted).
int PicHasPending(const PicDevice* pic);

#endif
