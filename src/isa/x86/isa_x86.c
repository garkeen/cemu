#include "x86.h"
#include "isa/isa.h"

const IsaOps kIsaX86 = {
    "x86",
    3,  // EM_386
    X86Init,
    X86Step,
    X86DumpRegs,
};
