#ifndef CEMU_CPU_STEP_H
#define CEMU_CPU_STEP_H

#include <setjmp.h>
#include <stdint.h>

#include "cpu/cpu.h"

// ============================================================================
// The seam between the machine layer and an interpreter. There is no
// intermediate representation: each interpreter is a plain giant switch that
// fetches, decodes and executes one instruction in a single C scope, reading
// like the architecture manual (case = manual page). What the layers DO share
// lives here: the per-step record (pc/next pc/raw bytes/mnemonic — the debug
// hub's observation surface), the raise channel, and the ops table that makes
// interpreters interchangeable.
//
// Step contract: the interpreter fills rec (pc, dnpc, raw, mnemonic) and
// either completes (cpu->pc committed to dnpc, exactly once per step) or
// raises (the ISA's raise consumer installed the resume point in dnpc and
// committed it). No partial state ever survives a faulting instruction.
// ============================================================================

// Per-instruction record, purely for observability (trace/state/mem rows).
typedef struct insn_rec {
  uint64_t pc, dnpc;
  uint8_t len;
  uint8_t raw[8];
  uint8_t raw_len;
  const char* mnemonic;
} insn_rec;

typedef struct trap_rec {
  uint64_t cause, tval;
} trap_rec;

// One step's world: the CPU, the ISA-private state, the record, and the
// raise landing pad. Interpreters take frame* so the debug hub watches every
// step through one type.
typedef struct frame {
  CpuState* cpu;
  void* priv;  // ISA-private state
  const struct isa_ops* isa;
  jmp_buf raise;  // raise_() lands here
  trap_rec trap;
  insn_rec rec;
} frame;

// Memory access classes (the riscv pipeline faults distinctly per class).
enum { acc_ifetch = 0, acc_read, acc_write, acc_amo };

// Raises out of the current instruction; the ISA's raise consumer installs
// the resume point in rec.dnpc. The whole step unwinds — the manual's
// instruction atomicity.
_Noreturn void raise_(frame* f, uint64_t cause, uint64_t tval);

// ---- the ops table ----------------------------------------------------------

// Line-id semantics are ISA-defined (riscv: a mip bit position; x86: ignored,
// one INTR line). Boards map device lines onto these ids at wiring time.
//
// Interrupt injection itself does NOT live here: it is the CpuState.set_irq
// hook, which the model installs in its init. A board wires its controllers
// to that hook, so wiring happens before the loader picks a CPU model.
typedef struct isa_ops {
  const char* name;
  uint32_t elf_machine;  // EM_* value recognized in ELF headers
  void (*init)(CpuState* cpu);
  // Fetch, decode, execute one instruction; commits cpu->pc exactly once.
  void (*step)(CpuState* cpu);
  void (*dump_regs)(const CpuState* cpu);

  // ---- debug hooks (AGENTS.md §X): optional, NULL = feature absent ----
  const char* (*cause_name)(uint64_t cause);  // cause id -> printable name
  const char* const* flag_names;              // set-bit names by flag-word bit, or NULL
  uint64_t (*flag_word)(frame* f);            // the flag word; 0 when flag_names is NULL
  int (*has_fpr)(frame* f);                   // state stream includes the FPR bank?
  void (*debug_state_line)(char* buf, int cap, frame* f);  // one-line summary
} isa_ops;

extern const isa_ops k_isa_x86;
extern const isa_ops k_isa_riscv64;

// Every ISA linked into the emulator registers itself here (NULL-terminated).
// Image loading picks an ISA by matching e_machine against elf_machine.
extern const isa_ops* const k_isa_table[];

#endif
