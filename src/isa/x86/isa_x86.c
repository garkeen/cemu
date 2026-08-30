#include "x86.h"
#include "isa/isa.h"

const IsaOps kIsaX86 = {
    "x86",
    3,  // EM_386
    0,  // raw bins use the PC boot-sector / multiboot contracts, not HTIF
    X86Init,
    X86Step,
    X86DumpRegs,
};
