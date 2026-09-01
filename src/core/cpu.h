#ifndef CEMU_CORE_CPU_H
#define CEMU_CORE_CPU_H

#include <stdint.h>

typedef struct Bus Bus;

enum { kCpuRunning = 0, kCpuExited = 1 };

typedef struct CpuState {
  uint64_t pc;
  uint64_t image_base;  // where the loaded image starts (multiboot scans here)
  Bus *bus;  // where the ISA reads and writes memory
  Bus *io;   // port I/O space for ISAs that have one (x86 IN/OUT); NULL = none
  void *priv;
  int halted;
  int wait;  // asleep waiting for an interrupt (wfi/hlt); the machine loop
             // keeps polling devices and sleeping the host while this is set
  int exit_code;
  uint64_t inst_count;
  // Machine time source for ISAs whose architecture reads wall-clock timers
  // (riscv time CSR); NULL = none. The ISA defines the semantics of the
  // value, the machine owns the clock.
  uint64_t (*timer_read)(void *timer_dev);
  void *timer_dev;
} CpuState;

#endif
