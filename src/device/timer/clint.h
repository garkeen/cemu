#ifndef CEMU_DEVICE_TIMER_CLINT_H
#define CEMU_DEVICE_TIMER_CLINT_H

#include "bus/bus.h"

// SiFive CLINT as instantiated by QEMU virt (DTB compatible "sifive,clint0",
// reg 0x02000000/0x10000): msip @+0x0 (bit 0 is the MSIP level), mtimecmp
// @+0x4000, mtime @+0xBFF8. mtime tracks the host monotonic clock scaled to
// the machine timebase (virt DTB /cpups/timebase-frequency = 10 MHz).
// Semantics follow dearchap-tinyemu riscv_machine.c clint_read/write: 64-bit
// registers accept any sub-word access; any mtimecmp write drops a pending
// MTIP. Two level-triggered output lines: 0 = MSIP, 1 = MTIP.
enum { kClintLineMsip = 0, kClintLineMtip = 1 };

typedef struct ClintDevice {
  uint64_t base;
  uint32_t msip;
  uint64_t mtimecmp;
  uint64_t mtime0;   // mtime value anchored at host time host0
  uint64_t host0;    // HostTimerNow() microseconds at anchor
  uint64_t timebase_hz;
  void (*set_irq)(void *ctx, int line, int level);
  void *irq_ctx;
} ClintDevice;

void ClintInit(ClintDevice *c, uint64_t timebase_hz);
void ClintRegister(Bus *bus, ClintDevice *c, uint64_t base, uint64_t size);
void ClintSetIrqSink(ClintDevice *c, void (*set_irq)(void *, int, int),
                     void *ctx);
// Current mtime; called by the machine poll loop and by the time CSR.
uint64_t ClintMtime(ClintDevice *c);
// Re-evaluates the MTIP line as host time advances past mtimecmp.
void ClintPoll(ClintDevice *c);

#endif
