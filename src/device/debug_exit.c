#include "device/debug_exit.h"

static uint64_t DebugExitRead(void *dev, uint64_t addr, int size) {
  (void)dev; (void)addr; (void)size;
  return 0;
}

static void DebugExitWrite(void *dev, uint64_t addr, int size, uint64_t val) {
  (void)addr; (void)size;
  DebugExitDevice *d = (DebugExitDevice *)dev;
  d->cpu->halted = kCpuExited;
  d->cpu->exit_code = (int)(val & 0xff) + 1;
}

const DeviceOps kDebugExitOps = {"debug-exit", DebugExitRead, DebugExitWrite};

void DebugExitBind(DebugExitDevice *d, CpuState *cpu) {
  d->cpu = cpu;
}
