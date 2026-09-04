#include "cpu/isa/isa.h"

// The single registration point for ISAs built into cemu. Adding another ISA
// (x86, arm, mips) appends it here.
const isa_ops* const k_isa_table[] = {&k_isa_riscv64, &k_isa_x86, NULL};
