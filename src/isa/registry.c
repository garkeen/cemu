#include "isa/isa.h"

// The single registration point for ISAs built into cemu. Adding another ISA
// (x86, arm, mips) appends it here.
const IsaOps *const kIsaTable[] = {&kIsaRiscv64, &kIsaX86, NULL};
