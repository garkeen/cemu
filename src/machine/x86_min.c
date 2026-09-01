#include <stdlib.h>
#include "machine/machine.h"
#include "device/uart16550.h"
#include "device/debug_exit.h"
#include "util/log.h"

// IBM PC real-mode machine, the x86 counterpart of spike_min: the smallest
// machine that runs bare 386 code. Memory is 1MB at linear 0 (real mode).
// COM1 sits at I/O ports 0x3F8-0x3FF and the exit device at port 0xF4, in a
// separate x86 I/O space (CpuState.io). Ports nobody claims read all-ones and
// drop writes — x86 I/O decode never faults.
static const uint64_t kRamSize = 1ULL << 20;
static const uint64_t kCom1Base = 0x3F8;
static const uint64_t kCom1Size = 8;
static const uint64_t kDebugExitPort = 0xF4;
static const uint64_t kDebugExitSize = 4;
static const uint64_t kPortSpaceSize = 0x10000;
// Boot-sector handoff (QEMU seabios): BIOS loads the image at 0x7C00 and
// enters it with CS:IP = 0000:7C00, DL = 0x80 (drive number).
static const uint64_t kBootSectorLoad = 0x7C00;

typedef struct X86Machine {
  Machine base;
  Uart16550 uart;
  DebugExitDevice dexit;
} X86Machine;

static uint64_t UnclaimedRead(void *dev, uint64_t addr, int size) {
  (void)dev; (void)addr;
  return size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
}

static void UnclaimedWrite(void *dev, uint64_t addr, int size, uint64_t val) {
  (void)dev; (void)addr; (void)size; (void)val;
}

static const DeviceOps kUnclaimedPortOps = {"unclaimed-ports", UnclaimedRead,
                                            UnclaimedWrite};

Machine *X86MachineCreate(const MachineOpts *opts) {
  if (opts->ram_base || opts->ram_size) {
    LogError("x86 machine has a fixed 1MB real-mode layout");
    return NULL;
  }
  X86Machine *xm = (X86Machine *)calloc(1, sizeof(X86Machine));
  if (!xm) return NULL;
  Machine *m = &xm->base;
  m->name = "x86";
  m->ram = RamCreate(0, kRamSize);
  if (!m->ram) {
    free(xm);
    return NULL;
  }
  BusAddRamRegion(&m->bus, 0, kRamSize, &kRamOps, m->ram, m->ram->mem);

  Uart16550 *uart = &xm->uart;
  DebugExitDevice *dexit = &xm->dexit;
  Uart16550Init(uart);
  DebugExitBind(dexit, &m->cpu);
  BusAddRegion(&m->io, 0, kPortSpaceSize, &kUnclaimedPortOps, NULL);
  BusAddRegion(&m->io, kCom1Base, kCom1Size, &kUart16550Ops, uart);
  BusAddRegion(&m->io, kDebugExitPort, kDebugExitSize, &kDebugExitOps, dexit);

  m->cpu.halted = kCpuRunning;
  m->cpu.bus = &m->bus;
  m->cpu.io = &m->io;
  m->bin_base = kBootSectorLoad;
  return m;
}
