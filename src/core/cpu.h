#ifndef CEMU_CORE_CPU_H
#define CEMU_CORE_CPU_H

#include <stdint.h>

typedef struct Bus Bus;

enum { kCpuRunning = 0, kCpuExited = 1 };

typedef struct CpuState {
  uint64_t pc;
  Bus *bus;  // where the ISA reads and writes memory
  Bus *io;   // port I/O space for ISAs that have one (x86 IN/OUT); NULL = none
  void *priv;
  int halted;
  int exit_code;
  uint64_t inst_count;
} CpuState;

#endif
