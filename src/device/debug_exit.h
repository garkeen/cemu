#ifndef CEMU_DEVICE_DEBUG_EXIT_H
#define CEMU_DEVICE_DEBUG_EXIT_H

#include <stdint.h>
#include "core/bus.h"
#include "core/cpu.h"

// QEMU isa-debug-exit: any write to its port ends emulation with status
// (value + 1), so status 0 is unrepresentable and "exited" is distinguishable
// from "never exited". kvm-unit-tests runs it at I/O port 0xF4.
typedef struct DebugExitDevice {
  CpuState *cpu;
} DebugExitDevice;

void DebugExitBind(DebugExitDevice *d, CpuState *cpu);

extern const DeviceOps kDebugExitOps;

#endif
