#ifndef CEMU_ISA_ISA_H
#define CEMU_ISA_ISA_H

#include "core/cpu.h"

// The seam between the generic core and any instruction set. The core talks
// only to this table; ISA directories never leak into core headers.
typedef struct IsaOps {
  const char *name;
  uint32_t elf_machine;   // EM_* value recognized in ELF headers
  void (*Init)(CpuState *cpu);
  void (*Step)(CpuState *cpu);
  void (*DumpRegs)(const CpuState *cpu);
} IsaOps;

extern const IsaOps kIsaRiscv64;

// Every ISA linked into the emulator registers itself here (NULL-terminated).
// Image loading picks an ISA by matching e_machine against ops->elf_machine.
extern const IsaOps *const kIsaTable[];

#endif
