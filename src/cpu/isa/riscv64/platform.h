#ifndef CEMU_CPU_ISA_RISCV64_PLATFORM_H
#define CEMU_CPU_ISA_RISCV64_PLATFORM_H

#include <stdint.h>

// The board-facing half of the CPU model: the interrupt line vocabulary a
// board wires its controllers to. A line IS a mip bit position (priv spec
// 3.1.9) — the board names it through cpu->set_irq and the model folds it
// into mip. This is the only header outside cpu/isa/ a board may include,
// and only a platform board (virt) needs it: generic boards (spike, PC) wire
// nothing that carries riscv line numbers.
//
// Only the implemented set appears: U-level sources have no generators on
// these boards, and H is absent.
static const uint64_t kIrqSsip = 1ULL << 1;
static const uint64_t kIrqMsip = 1ULL << 3;
static const uint64_t kIrqStip = 1ULL << 5;
static const uint64_t kIrqMtip = 1ULL << 7;
static const uint64_t kIrqSeip = 1ULL << 9;
static const uint64_t kIrqMeip = 1ULL << 11;

#endif
