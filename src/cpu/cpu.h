#ifndef CEMU_CPU_CPU_H
#define CEMU_CPU_CPU_H

#include <stdint.h>

typedef struct Bus Bus;

enum { kCpuRunning = 0, kCpuExited = 1 };

// The architectural CPU state: register banks plus the committed PC and the
// run-control bits. One bank layout serves every ISA — riscv64 uses all 32
// GPR cells, x86 uses cells 0..7 (the 32-bit registers; the upper halves of
// the 64-bit cells stay zero). Sub-register access (AH, the 16-bit forms)
// goes through the ISA's own union views over its bank (x86.h `cell`), never
// through index arithmetic. The ISA-private state (CSRs, segments) hangs off
// `priv`. Devices and boards see only this struct; the model behind `priv` is
// not visible outside cpu/isa/.
typedef struct CpuState {
  uint64_t gpr[32];   // GPR bank; cells addressed by index
  uint64_t fpr[32];   // FPR bank (riscv F/D, NaN-boxed); x86 leaves it unused
  uint64_t pc;        // the one committed PC (address space is ISA-defined:
                      // riscv = linear vaddr; x86 = offset within CS)
  uint64_t image_base;  // where the loaded image starts (multiboot scans here)
  Bus *bus;  // where the ISA reads and writes memory
  Bus *io;   // port I/O space for ISAs that have one (x86 IN/OUT); NULL = none
  void *priv;
  int halted;
  int wait;  // asleep waiting for an interrupt (wfi/hlt); the board run loop
             // keeps polling devices and sleeping the host while this is set
  int exit_code;
  uint64_t inst_count;
  // Board time source for ISAs whose architecture reads wall-clock timers
  // (riscv time CSR); NULL = none. The ISA defines the semantics of the
  // value, the board owns the clock.
  uint64_t (*timer_read)(void *timer_dev);
  void *timer_dev;
  // Interrupt-acknowledge cycle for ISAs with a hardware interrupt
  // handshake (x86 INTA): returns the pending vector number. The board wires
  // this to its interrupt controller; NULL = no external irq source.
  int (*int_ack)(void *ack_dev);
  void *ack_dev;
  // Interrupt injection: a device line changed level. Installed by the CPU
  // model's init (riscv: mip bit positions; x86: one INTR line, `line`
  // ignored). Boards wire their controllers to this hook instead of calling
  // an ISA function, so a board stays free of CPU-model knowledge.
  void (*set_irq)(struct CpuState *cpu, uint64_t line, int level);
} CpuState;

#endif
