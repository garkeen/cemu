#ifndef CEMU_CPU_ISA_X86_X86_H
#define CEMU_CPU_ISA_X86_X86_H

#include <stdint.h>

#include "cpu/cpu.h"

// The x86 interpreter, written to read like the manual: each case is one
// instruction, operand names are the manual's names (r/m8, reg8, imm8...),
// and the width lives in the function-name suffix. Scope: real mode plus
// protected mode through segment protection (SDM vol.3 5.3: descriptor
// parse, CPL/RPL/DPL and limit checks), the gate machinery (interrupt/trap/
// call/task gates, SDM vol.3 6-7), task switching, two-level paging
// (SDM vol.3 4) and the LDT through its descriptor cache (SDM vol.3 2.4.4,
// 3.5); the descriptor cache surviving CR0.PE=0 IS big real mode.

// Register cells follow the modrm reg/rm encoding order (SDM table 3-1).
enum { eax_i, ecx_i, edx_i, ebx_i, esp_i, ebp_i, esi_i, edi_i };
// Segment indices follow the sreg 3-bit encoding order.
enum { es_i, cs_i, ss_i, ds_i, fs_i, gs_i };
// Exception vectors (SDM vol.3 table 6-1).
enum {
  vec_de = 0, vec_db = 1, vec_bp = 3, vec_ud = 6, vec_nm = 7, vec_df = 8, vec_ts = 10,
  vec_np = 11, vec_ss = 12, vec_gp = 13, vec_pf = 14, vec_ac = 17
};
// Where a delivered vector came from (SDM vol.2 INT Operation / vol.3 table
// 6-1): only a CPU-raised exception pushes the vector's error code — a software
// INT n and the external interrupt pin push a bare flags/CS/IP frame.
enum { kIntException = 0, kIntExternal = 1, kIntSoft = 2 };

// Debug registers (SDM vol.3 ch.17). DR6's reserved bits read 1 (reset
// 0xffff0ff0) and its B bits clear on any write; DR7 bit 10 reads 1 (reset
// 0x400). R/Wi live at DR7[17:16+2i] and LENi at DR7[19:18+2i] (table 17-2);
// LEN encodes 1/2/4/8 bytes (QEMU/KVM honor the 8-byte encoding in 32-bit
// mode, and the kvm debug test arms LEN=11).
enum {
  kDr6B0 = 1u, kDr6Bd = 1u << 13, kDr6Bs = 1u << 14, kDr6Bt = 1u << 15,
  kDr6Rsvd1 = 0xffff0ff0u,
  kDr7Le = 1u << 8, kDr7Ge = 1u << 9, kDr7Gd = 1u << 13, kDr7Rsvd1 = 0x400u
};

// CR0 flags (SDM vol.3 2.5). PE drives protected mode and PG the page
// tables; WP makes supervisor stores honor read-only pages (486+; wired
// because SDM vol.3 4.6 defines the flag); TS/EM/MP ride along for
// LMSW/CLTS.
enum { kCr0Pe = 1u, kCr0Mp = 2u, kCr0Em = 4u, kCr0Ts = 8u, kCr0Wp = 1u << 16,
       kCr0Pg = 1u << 31 };
enum { kCr4Vme = 1u, kCr4Pvi = 2u, kCr4Tsd = 4u, kCr4De = 8u, kCr4Pse = 0x10u };
// Page-directory / page-table entry flags for 4KB pages (SDM vol.3 4.3):
// P, R/W, U/S and A exist at both levels, D only in the leaf. PS lives in
// the PDE alone and turns it into a 4MB page when CR4.PSE=1 (vol.3 4.3).
enum { kPdeP = 1u, kPdeRw = 2u, kPdeUs = 4u, kPdeA = 0x20u, kPdeD = 0x40u,
       kPdePs = 0x80u };
enum { kPteP = 1u, kPteRw = 2u, kPteUs = 4u, kPteA = 0x20u, kPteD = 0x40u };

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
//
// Bits 1, 3, 5 and 15 are constants of the register rather than state — bit 1
// reads 1, the rest read 0 (SDM vol.1 3.4.3 table 3-1) — so no image loaded
// from the stack or a TSS can carry them; the step commit forces them.
enum { kEflagsOne = 0x2u, kEflagsZero = 0x8028u };
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
  uint32_t cr2;       // #PF stores the faulting linear address here (SDM vol.3 2.5)
  uint32_t cr3;       // page-directory base (PDBR); carried by task switches
                      // (TSS +1c); the walk uses bits 31:12
  uint32_t cr4;       // PSE gates 4MB pages (SDM vol.3 2.5; 486+/Pentium)
  uint64_t msr_apic_base;  // MSR 0x1B image; the MMIO region itself is fixed
  uint64_t msr_fs_gs_base[2];  // 0xc0000100/101: written by the kvm-unit-tests
                               // boot (cstart setup_percpu_area), no effect
                               // without long mode — read back per QEMU
  uint64_t gdtr;
  uint16_t gdtr_limit;
  uint64_t idtr;
  uint16_t idtr_limit;
  uint16_t tr;        // visible TR selector (LTR/STR)
  uint64_t tr_base;   // task register descriptor cache (SDM vol.3 7.2)
  uint32_t tr_limit;
  uint8_t tr_ar;      // access byte; type bit 3 distinguishes 32-bit TSS
  uint16_t ldtr;      // visible LDTR selector; 0 = no LDT (SDM vol.3 2.4.4)
  uint64_t ldtr_base; // LDT descriptor cache: TI=1 selector lookups and
  uint32_t ldtr_limit;  // VERR/VERW/LAR/LSL read through it
  uint32_t dr[8];
  int bt_pending;    // the incoming task's TSS.T: deliver #DB before its
                     // first instruction (SDM vol.3 7.2.1)
  int a20;           // A20 gate line state: bit 20 of every physical address is
                     // forced low while it is off (PC/AT); the board drives it
                     // through CpuState.set_a20
  int intr_pending;  // the machine's INTR line is asserted
  int intr_inhibit;  // SDM window: instruction after STI takes no INTR
  // gdb stub (stage 3.5): bumped on every delivered exception (the INTR
  // delivery path does NOT count — a hardware interrupt is not a debug
  // event); signal is the gdb mapping of the last vector (step.c).
  uint64_t trap_seq;
  uint8_t trap_signal;
} x86_state;

void x86_init(CpuState* cpu);
void x86_step(CpuState* cpu);
void x86_dump_regs(const CpuState* cpu);
void x86_set_intr(CpuState* cpu, int level);

#endif
