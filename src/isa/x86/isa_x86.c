#include "x86.h"
#include "isa/isa.h"

// External INTR line (machine irq sink -> CPU). Level 1 asserts; the vector
// is fetched from the INTA hook when X86Step dispatches it.
static void X86RaiseIrq(CpuState *cpu, int line, int level) {
  (void)line;  // one shared INTR line; the PIC supplies the vector on ack
  X86SetIntr(cpu, level);
}

const IsaOps kIsaX86 = {
    "x86",
    3,  // EM_386
    X86Init,
    X86Step,
    X86DumpRegs,
    X86RaiseIrq,
};
