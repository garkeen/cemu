#ifndef CEMU_DEVICE_MISC_DEBUG_EXIT_H
#define CEMU_DEVICE_MISC_DEBUG_EXIT_H

#include <stdint.h>

#include "bus/bus.h"
#include "cpu/cpu.h"

// QEMU isa-debug-exit (hw/misc/debugexit.c): any write to its port ends
// emulation with status (value << 1) | 1 — always odd, so "exited" is
// distinguishable from "never exited". kvm-unit-tests runs it at I/O port
// 0xF4.
typedef struct DebugExitDevice {
  CpuState* cpu;
} DebugExitDevice;

void DebugExitBind(DebugExitDevice* d, CpuState* cpu);

extern const DeviceOps kDebugExitOps;

#endif
