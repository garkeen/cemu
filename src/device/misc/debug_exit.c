#include "device/misc/debug_exit.h"

static uint64_t DebugExitRead(void* dev, uint64_t addr, int size) {
  (void)dev;
  (void)addr;
  (void)size;
  return 0;
}

static void DebugExitWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)addr;
  (void)size;
  DebugExitDevice* d = (DebugExitDevice*)dev;
  // QEMU hw/misc/debugexit.c: status = (value << 1) | 1, calibrated against
  // qemu-system-i386 -device isa-debug-exit (payloads 0/2/5 -> 1/5/11).
  d->cpu->halted = kCpuExited;
  d->cpu->exit_code = (int)(((val & 0xff) << 1) | 1);
}

const DeviceOps kDebugExitOps = {"debug-exit", DebugExitRead, DebugExitWrite};

void DebugExitBind(DebugExitDevice* d, CpuState* cpu) { d->cpu = cpu; }
