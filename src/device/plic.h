#ifndef CEMU_DEVICE_PLIC_H
#define CEMU_DEVICE_PLIC_H

#include "core/bus.h"

// SiFive PLIC 1.0.0 as instantiated by QEMU virt (DTB compatible
// "sifive,plic-1.0.0", riscv,ndev = 95, contexts = hart0 M-mode then S-mode).
// Register map per the SiFive PLIC manual, matching QEMU hw/intc/sifive_plic:
//   priority[src]  @ +0x000000 + 4*src          (src 1..95, RW)
//   pending        @ +0x001000                   (RO)
//   enable[ctx]    @ +0x002000 + 0x80*ctx        (RW, 3 words used)
//   threshold[ctx] @ +0x200000 + 0x1000*ctx      (RW)
//   claim/complete @ threshold + 4               (claim = read, complete = write)
// Claim returns the highest-priority enabled pending source above the context
// threshold (lowest source number on ties), clears its pending bit, and
// returns it; complete is a no-op beyond re-evaluating outputs (QEMU
// semantics; dearchap-tinyemu riscv_machine.c is the C reference shape).
// Output lines are level driven: line ctx = "context ctx has a claimable
// interrupt".
enum { kPlicNumSrc = 95, kPlicNumCtx = 2, kPlicNumWords = 3 };

typedef struct PlicDevice {
  uint64_t base;
  uint32_t priority[kPlicNumSrc + 1];            // [0] reserved, reads 0
  uint32_t pending[kPlicNumWords];               // bit i = source i
  uint32_t enable[kPlicNumCtx][kPlicNumWords];
  uint32_t threshold[kPlicNumCtx];
  void (*set_irq)(void *ctx, int line, int level);
  void *irq_ctx;
} PlicDevice;

void PlicInit(PlicDevice *p);
void PlicRegister(Bus *bus, PlicDevice *p, uint64_t base, uint64_t size);
void PlicSetIrqSink(PlicDevice *p, void (*set_irq)(void *, int, int),
                    void *ctx);
// Device-side input: asserts/drops external source src (level).
void PlicDeviceIrq(PlicDevice *p, int src, int level);

#endif
