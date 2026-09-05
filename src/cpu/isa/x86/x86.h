#ifndef CEMU_CPU_ISA_X86_X86_H
#define CEMU_CPU_ISA_X86_X86_H

#include <stdint.h>

#include "cpu/cpu.h"

// The x86 interpreter, written to read like the manual: each case is one
// instruction, operand names are the manual's names (r/m8, reg8, imm8...),
// and the width lives in the function-name suffix. Scope: real mode plus
// protected mode through segment protection (SDM vol.3 5.3: descriptor
// parse, CPL/RPL/DPL and limit checks), the gate machinery (interrupt/trap/
// call/task gates, SDM vol.3 6-7) and task switching; paging is not in yet.
// The descriptor cache surviving CR0.PE=0 IS big real mode.

// Register cells follow the modrm reg/rm encoding order (SDM table 3-1).
enum { eax_i, ecx_i, edx_i, ebx_i, esp_i, ebp_i, esi_i, edi_i };
// Segment indices follow the sreg 3-bit encoding order.
enum { es_i, cs_i, ss_i, ds_i, fs_i, gs_i };
// Exception vectors (SDM vol.3 table 6-1).
enum {
  vec_de = 0, vec_ud = 6, vec_df = 8, vec_ts = 10, vec_np = 11,
  vec_ss = 12, vec_gp = 13, vec_pf = 14, vec_ac = 17
};

// One general-purpose bank cell, named exactly the way the architecture
// names its parts: a 32-bit register, whose low half is the 16-bit one,
// whose low byte and its high neighbor are the 8-bit pair (SDM 3.4.1).
typedef union cell {
  uint64_t q;
  // The x86 register aliasing is layered views of one cell, and unions are
  // how layering is written in C: EAX is the 32-bit view, AX the 16-bit view
  // of its low half, AL/AH the two bytes of AX — every name starts at offset
  // zero, so writing a narrow view leaves the wider upper bits alone.
  union {
    uint32_t e;
    union {
      uint16_t x;
      struct {
        uint8_t l, h;
      };
    };
  };
} cell;

// EFLAGS as the manual presents it: whole word for pushf/popf/iret, single
// flags everywhere else. Bit 1 is reserved-one (never named); the PM-relevant
// upper bits (IOPL, NT) join the named set once gates load a flags image.
typedef union eflags {
  uint32_t word;
  struct {
    unsigned cf : 1, : 1, pf : 1, : 1, af : 1, : 1, zf : 1, sf : 1;
    unsigned tf : 1, if_ : 1, df : 1, of : 1, iopl : 2, nt : 1, : 1, rf : 1, vm : 1, rest : 14;
  };
} eflags;

typedef struct x86_state {
  cell* r;           // the shared CpuState bank, seen as eight cells
  uint16_t sreg[6];  // visible selector values
  uint64_t base[6];  // descriptor-cache segment bases
  uint32_t limit[6]; // descriptor-cache effective limits (G already expanded)
  uint8_t ar[6];     // descriptor-cache access rights: P DPL S Type
  uint8_t dbit[6];   // D/B flag: default operand size is 4 when set
  eflags fl;
  uint32_t cr0;
  uint32_t cr3;       // page-table base: carried by task switches (TSS +1c);
                      // paging itself is stage-3 item 4
  uint64_t gdtr;
  uint16_t gdtr_limit;
  uint64_t idtr;
  uint16_t idtr_limit;
  uint16_t tr;        // visible TR selector (LTR/STR)
  uint64_t tr_base;   // task register descriptor cache (SDM vol.3 7.2)
  uint32_t tr_limit;
  uint8_t tr_ar;      // access byte; type bit 3 distinguishes 32-bit TSS
  uint32_t dr[8];
  int intr_pending;  // the machine's INTR line is asserted
  int intr_inhibit;  // SDM window: instruction after STI takes no INTR
} x86_state;

void x86_init(CpuState* cpu);
void x86_step(CpuState* cpu);
void x86_dump_regs(const CpuState* cpu);
void x86_set_intr(CpuState* cpu, int level);

#endif
