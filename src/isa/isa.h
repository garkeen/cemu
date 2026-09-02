#ifndef CEMU_ISA_ISA_H
#define CEMU_ISA_ISA_H

#include "core/cpu.h"

// The seam between the generic core and any instruction set. The core talks
// only to this table; ISA directories never leak into core headers.
typedef struct IsaOps {
  const char *name;
  uint32_t elf_machine;   // EM_* value recognized in ELF headers
  void (*Init)(CpuState *cpu);
  void (*Step)(CpuState *cpu);                        // 取指译码执行一条
  void (*DumpRegs)(const CpuState *cpu);
  // External interrupt input. `line` is an ISA-defined interrupt id (e.g. an
  // IRQ number); `level` is 0/1. Devices call this through the machine's irq
  // sink to feed interrupts into the ISA's dispatch. NULL = no external irq
  // path on this ISA (spike's HTIF is not an interrupt source).
  void (*RaiseIrq)(CpuState *cpu, int line, int level);
} IsaOps;

extern const IsaOps kIsaRiscv64;
extern const IsaOps kIsaX86;

// Every ISA linked into the emulator registers itself here (NULL-terminated).
// Image loading picks an ISA by matching e_machine against ops->elf_machine.
extern const IsaOps *const kIsaTable[];

#endif
