#ifndef CEMU_CPU_ISA_X86_X86_H
#define CEMU_CPU_ISA_X86_X86_H

#include <stdint.h>

#include "cpu/cpu.h"
#include "cpu/isa/x86/fpu.h"

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
  vec_de = 0, vec_db = 1, vec_nmi = 2, vec_bp = 3, vec_br = 5, vec_ud = 6, vec_nm = 7,
  vec_df = 8,
  vec_ts = 10, vec_np = 11, vec_ss = 12, vec_gp = 13, vec_pf = 14, vec_mf = 16,
  vec_ac = 17, vec_mc = 18, vec_xm = 19
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
// DR7's per-breakpoint fields, as positions rather than as shifts written at
// each use: Li at bit 2i, R/Wi at [17:16+4i], LENi at [19:18+4i] (SDM vol.3
// figure 17-3). The stride is 4 because one breakpoint owns two 2-bit fields.
enum {
  kDr7LBase = 0, kDr7RwBase = 16, kDr7LenBase = 18,
  kDr7AnyEnable = 0xffu,  // both enable bits (Li/Gi) of all four slots
  // The R/W encodings (SDM vol.3 17.2.4, table 17-2). There is no read-only
  // data breakpoint: 11 means read *or* write.
  kDr7RwExec = 0, kDr7RwWrite = 1, kDr7RwIo = 2, kDr7RwAccess = 3
};
// LENi is a code, not a byte count: 00/01/10/11 = 1/2/4/8 bytes.
enum { kDrLenCount = 4, kDrCount = 4 };

// CR0 flags (SDM vol.3 2.5). PE drives protected mode and PG the page
// tables; WP makes supervisor stores honor read-only pages (486+; wired
// because SDM vol.3 4.6 defines the flag); TS/EM/MP ride along for
// LMSW/CLTS.
enum { kCr0Pe = 1u, kCr0Mp = 2u, kCr0Em = 4u, kCr0Ts = 8u, kCr0Ne = 1u << 5,
       kCr0Wp = 1u << 16, kCr0Am = 1u << 18, kCr0Pg = 1u << 31 };
// CR4: PSE gates 4MB pages; PAE selects the three-level 64-bit-entry walk;
// PGE makes global translations survive a CR3 reload (SDM vol.3 2.5, 4.3,
// 4.10.4.1). DE/TSD/VME/PVI are defined so the register reads back honestly;
// the behaviours this machine does not have yet are registered in AGENTS.md.
enum {
  kCr4Vme = 1u,
  kCr4Pvi = 2u,
  kCr4Tsd = 4u,
  kCr4De = 8u,
  kCr4Pse = 0x10u,
  kCr4Pae = 0x20u,
  kCr4Pge = 0x80u,
  kCr4Osfxsr = 0x200u,     // FXSAVE/FXRSTOR + SSE state enable (SDM 13.5)
  kCr4Osxmmexcpt = 0x400u, // SSE unmasked exceptions report #XM
};
// The CR4 bits this machine implements. A write is WARL: unsupported bits
// read back zero (SDM vol.3 2.5 "CR4 ... may be ... reserved").
enum { kCr4Supported = kCr4Vme | kCr4Pvi | kCr4Tsd | kCr4De | kCr4Pse | kCr4Pae |
                         kCr4Pge | kCr4Osfxsr | kCr4Osxmmexcpt };
// IA32_EFER (MSR 0xC0000080): only NX enable is meaningful without long mode.
enum { kEferNxe = 1ull << 11 };
// Page-directory / page-table entry flags for 4KB pages (SDM vol.3 4.3):
// P, R/W, U/S and A exist at both levels, D only in the leaf. PS lives in
// the PDE alone and turns it into a 4MB page when CR4.PSE=1 (vol.3 4.3).
enum { kPdeP = 1u, kPdeRw = 2u, kPdeUs = 4u, kPdeA = 0x20u, kPdeD = 0x40u,
       kPdePs = 0x80u, kPdeG = 0x100u };
enum { kPteP = 1u, kPteRw = 2u, kPteUs = 4u, kPteA = 0x20u, kPteD = 0x40u,
       kPteG = 0x100u };
// #PF error-code bits beyond the 386 set (SDM vol.3 table 6-1): RSVD and I
// appear with PAE/NX. The non-PAE 386-class code keeps exactly P|W|U.
enum { kPfPresent = 1u, kPfWrite = 2u, kPfUser = 4u, kPfRsvd = 8u, kPfInstr = 0x10u };
// A 4-MByte page with PSE-36 keeps bits 31:22 in the PDE, takes physical
// address bits 39:32 from PDE[20:13], and leaves PDE[21] reserved (SDM vol.3
// fig 4-4). A set reserved bit is a page fault; the 386-class #PF error code
// carries no RSVD flag, so the fault reads as an ordinary present fault.
enum { kPde4mReserved = 0x00200000u };
// SGDT/SIDT/LGDT/LIDT with a 16-bit operand size keep only 24 base bits.
enum { kDescBase16Mask = 0x00ffffffu };

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
    unsigned tf : 1, if_ : 1, df : 1, of : 1, iopl : 2, nt : 1, : 1, rf : 1, vm : 1;
    unsigned ac : 1, vif : 1, vip : 1, id : 1, rest : 10;
  };
} eflags;

// The unified translation cache. One read and one write slot per set: a read
// fill must not satisfy a later write, or the page's D bit would never be set
// (the pm_smoke A/D test and KUT access rely on the write walk doing that).
// Tags are linear page numbers; CR3 changes flush non-global entries, and
// INVLPG flushes a page, so no ASID is needed on a 32-bit machine.
enum {
  kTlbSets = 4096,
  kTlbWays = 2,
  kTlbValid = 1u << 31,
  // Bits 30:21 of a tag hold a 10-bit flush generation: TlbFlushAll bumps it
  // instead of clearing the whole cache, and a tag only matches while its
  // generation equals the live one (wraparound clears the array).
  kTlbGenShift = 21,
  kTlbGenMask = 0x3ffu,
  kTlbWritable = 1u << 0,
  kTlbUser = 1u << 1,
  kTlbNx = 1u << 2,
  kTlbGlobal = 1u << 3,
  kTlbHost = 1u << 4,
};
typedef struct x86_tlb_entry {
  uint32_t tag;    // kTlbValid | (linear page << 1) | write
  uint32_t flags;  // kTlbWritable/User/Nx/Global/Host
  uint32_t lin_base;   // start of the mapped page (4KB, 2MB or 4MB)
  uint32_t page_shift; // 12, 21 or 22: INVLPG must drop the whole range
  uint64_t pbase;      // physical page base, A20 already applied
  uint8_t* host;       // page start when kTlbHost
} x86_tlb_entry;

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
                      // (TSS +1c); the walk uses bits 31:12 (PAE: 31:5)
  uint32_t cr4;       // PSE/PGE select 4MB pages and global translations;
                      // PAE selects the three-level walk (SDM vol.3 2.5)
  uint64_t efer;      // IA32_EFER: NXE (bit 11) enables the PAE NX bit
  x86_fpu fpu;        // x87 stack, control/status and the SSE state (fpu.h)
  uint64_t msr_apic_base;  // MSR 0x1B image; the MMIO region itself is fixed
  uint64_t msr_fs_gs_base[2];  // 0xc0000100/101: written by the kvm-unit-tests
                               // boot (cstart setup_percpu_area), no effect
                               // without long mode — read back per QEMU
  // IA32_TSC (0x10) is read as elapsed emulated time plus the instruction
  // count; a WRMSR stores the difference from that base so the next RDTSC
  // continues from the written value (SDM vol.3 17.17).
  uint64_t tsc_offset;
  uint32_t tsc_aux;   // IA32_TSC_AUX (0xc0000103), returned by RDTSCP
  // SYSENTER/SYSEXIT state (SDM vol.2 SYSENTER/SYSEXIT; MSRs 0x174-0x176).
  uint32_t sysenter_cs, sysenter_esp, sysenter_eip;
  uint64_t mtrr_physbase[8], mtrr_physmask[8];
  uint64_t mtrr_def_type;  // IA32_MTRR_DEF_TYPE (0x2ff)
  uint64_t pat;            // IA32_PAT (0x277); no cache model consumes it
  // Machine-check registers: IA32_MCG_* plus bank 0's four registers. This
  // machine has no error source that raises #MC; the registers are the
  // architectural probe/reset interface (SDM vol.3 ch.15).
  uint64_t mcg_status, mcg_ctl;
  uint64_t mc0_ctl, mc0_status, mc0_addr, mc0_misc;
  // Architectural performance counters (SDM vol.3 ch.19): two general
  // counters plus the three fixed ones. Counted per retired instruction.
  uint64_t perf_evtsel[2], perf_pmc[2];
  uint64_t perf_fixed_ctrl, perf_global_ctrl, perf_global_status, perf_global_ovf;
  uint64_t perf_fixed_ctr[3];
  // Bit i = general counter i; bit 2+i = fixed counter i. Rebuilt whenever a
  // controlling MSR changes, so the step commit tests one word before the
  // counter loop (the hot path for every guest that never enables PMU).
  uint32_t perf_active;
  // Translation cache and the single-page fetch shortcut. Both are
  // architecture-invisible caches; every TLB invalidation also drops the
  // fetch shortcut. The fetch pointer aims at RAM, so a guest writing code
  // sees its own bytes (no I-cache to invalidate).
  x86_tlb_entry tlb[kTlbSets * kTlbWays];
  uint32_t tlb_gen;  // current flush generation (kTlbGenMask + 1 states)
  uint32_t fetch_page;  // linear page the shortcut describes
  uint8_t* fetch_host;
  int fetch_valid;
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
  // Which of the four debug registers the gdb stub holds (one bit per slot, set
  // by x86_gdb_set_watch and cleared by x86_gdb_clear_watch), and the slot bits
  // that fired on the step now committing. The distinction is ownership, not a
  // second matcher: a watchpoint the debugger armed is reported to the debugger
  // and not vectored as a guest fault; ones the guest programmed for itself
  // vector normally (step.c's debug-trap block).
  uint32_t gdb_watch_mask;
  uint32_t watch_hits;
  int dr_active;     // DR7 has at least one breakpoint enable (step.c fast gate)
  int bt_pending;    // the incoming task's TSS.T: deliver #DB before its
                     // first instruction (SDM vol.3 7.2.1)
  int ferr_level;    // FERR# line state when CR0.NE=0 (the board drives IRQ13)
  int a20;           // A20 gate line state: bit 20 of every physical address is
                     // forced low while it is off (PC/AT); the board drives it
                     // through CpuState.set_a20
  int intr_pending;  // the machine's INTR line is asserted
  int intr_inhibit;  // SDM window: instruction after STI takes no INTR
  int nmi_pending;   // the machine's NMI pin has been asserted
  int nmi_blocked;   // an NMI is being serviced: held until the next IRET
                     // (SDM vol.3 6.7.1)
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
void x86_set_nmi(CpuState* cpu);
// The debug-register door the gdb stub's hardware watchpoints use (exec.c's
// debug-register section): installing through MOV DRn and through RSP is the same
// machine state, so there is one matcher and one #DB. A slot index comes back so
// the caller removes exactly what it put in; -1 means the encoding cannot say it.
int x86_gdb_set_watch(CpuState* cpu, uint64_t addr, uint32_t len, int kind);
int x86_gdb_clear_watch(CpuState* cpu, int slot);

#endif
