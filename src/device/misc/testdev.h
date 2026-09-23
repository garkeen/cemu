#ifndef CEMU_DEVICE_MISC_TESTDEV_H
#define CEMU_DEVICE_MISC_TESTDEV_H

#include <stdint.h>

#include "bus/bus.h"

// The kvm-unit-tests test device's interrupt-injection window: a write to I/O
// port 0x2000 + line drives that ISA IRQ line (non-zero asserts, zero
// deasserts), so a guest can key the controllers from software. x86/ioapic.c
// uses it to exercise the I/O APIC's redirection entries:
//
//   asm volatile("out %0, %1" : : "a"((u8)val), "d"((u16)(0x2000 + line)));
//
// Reference: v86 src/cpu.js ("only for kvm-unit-test": for i in 0..0xF a write
// to 0x2000 + i calls device_raise_irq(i) / device_lower_irq(i), the same
// handler for every width). The line is a machine wire rather than this
// device's own state: the board's IRQ fan-out takes it, and that fan-out
// reaches both controllers, so either can be the one that delivers.
enum { kTestDevBase = 0x2000 };
enum { kTestDevLines = 16 };

typedef struct TestDevDevice {
  void (*set_irq)(void* ctx, int line, int level);
  void* irq_ctx;
} TestDevDevice;

void TestDevInit(TestDevDevice* d);
void TestDevRegister(Bus* io, TestDevDevice* d);
void TestDevSetIrqSink(TestDevDevice* d, void (*set_irq)(void* ctx, int line, int level), void* ctx);

extern const DeviceOps kTestDevOps;

#endif
