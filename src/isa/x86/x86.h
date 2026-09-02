#ifndef CEMU_ISA_X86_X86_H
#define CEMU_ISA_X86_X86_H

#include "core/cpu.h"
#include "core/bus.h"

// 386 real-mode interpreter. Scope: real mode plus the protected-mode entry
// the multiboot boot contract requires (flat segments via lgdt/far jumps and
// CR0.PE, no paging, no privilege checks, no PM exceptions). Segment limits
// are not enforced, which is what makes "big real mode" (a descriptor cache
// carried across CR0.PE=0) work naturally.

// General register indices follow the modrm reg/rm encoding order.
enum { kEax = 0, kEcx, kEdx, kEbx, kEsp, kEbp, kEsi, kEdi, kGprCount };
// Segment indices follow the sreg 3-bit encoding order.
enum { kSegEs = 0, kSegCs, kSegSs, kSegDs, kSegFs, kSegGs, kSegCount };

// EFLAGS bits (386 real-mode set).
enum {
  kFlagCf = 1 << 0,
  kFlagPf = 1 << 2,
  kFlagAf = 1 << 4,
  kFlagZf = 1 << 6,
  kFlagSf = 1 << 7,
  kFlagTf = 1 << 8,
  kFlagIf = 1 << 9,
  kFlagDf = 1 << 10,
  kFlagOf = 1 << 11,
  kFlagRf = 1 << 16,  // resume flag: transient, never survives to pushf
  kFlagVm = 1 << 17,  // VM86 mode: not entered in this model
};

enum { kCr0Pe = 1 << 0 };  // protection enable

typedef struct X86State {
  uint32_t gpr[kGprCount];
  uint16_t sreg[kSegCount];   // visible selector values
  uint64_t base[kSegCount];   // protected-mode descriptor bases
  uint8_t dbit[kSegCount];    // D/B flag: default operand size is 4 when set
  uint32_t eip;
  uint32_t eflags;
  uint32_t cr0;
  uint64_t gdtr;              // linear base of the guest GDT
  uint16_t gdtr_limit;
  uint64_t idtr;
  uint16_t idtr_limit;
  uint32_t dr[8];
  // External interrupt input. The machine's irq sink raises the INTR line
  // here (X86SetIntr); X86Step acknowledges and dispatches it when IF=1.
  // The vector comes from CpuState.int_ack (machine wires it to the PIC).
  int intr_pending;
  // SDM interrupt-inhibit window: the instruction after STI/POP SS/MOV SS
  // does not sample INTR. Cleared when that instruction retires.
  int intr_inhibit;
} X86State;

void X86Init(CpuState *cpu);
void X86Step(CpuState *cpu);
void X86DumpRegs(const CpuState *cpu);
// Machine-side irq sink: asserts/deasserts the external INTR line.
void X86SetIntr(CpuState *cpu, int level);

#endif
