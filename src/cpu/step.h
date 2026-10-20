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

  // ---- gdb stub hooks (stage 3.5): optional, NULL = feature absent ----
  // The register file in the gdb target-description order, little-endian:
  // read fills buf (cap bytes) and returns the image length, write applies a
  // full image; 0/-1 = unsupported. x86 segment registers follow QEMU's
  // gdbstub: the visible selector is stored, the descriptor caches are not
  // re-walked (a blind descriptor load has no fault channel from a debugger).
  int (*gdb_read_regs)(CpuState* cpu, uint8_t* buf, int cap);
  int (*gdb_write_regs)(CpuState* cpu, const uint8_t* buf, int len);
  // The most recently delivered exception: writes the trap's sequence number
  // (bumped on every delivery) and returns the gdb signal for it, 0 = none.
  // The stub detects "the step that just ran delivered a trap" by comparing
  // sequence numbers across the step.
  int (*gdb_last_trap)(CpuState* cpu, uint64_t* seq);
  // The program counter as gdb and the stub must see it: the linear address,
  // the same space the stub's memory packets (m/M) read and write. Handing gdb
  // the offset instead breaks real-mode debugging in both directions — `x/i
  // $pc` disassembles bytes taken from the wrong place whenever CS has a
  // non-zero base, and a breakpoint at the address gdb can actually name never
  // fires. NULL = linear and the committed pc are the same thing.
  uint64_t (*gdb_pc)(CpuState* cpu);
  // A hardware watchpoint requested through gdb's RSP `Z2`/`Z3`/`Z4` packet,
  // installed in the processor's own debug registers (x86: DR0-DR7, SDM
  // vol.3 17.2) rather than in a second matcher: the same machine state a guest
  // debugger programs with MOV DRn, so the trap that stops the session is the one
  // the CPU already raises. `kind` is the RSP kind (2 = write, 3 = read,
  // 4 = access) and `len` a byte count -- the mapping to the architecture's
  // encoding is the ISA's, and whatever it cannot express it refuses here: x86
  // has no read-only data breakpoint (R/W 11 means read OR write, SDM vol.3
  // table 17-2). Returns the debug register slot used, or -1 -- no free slot, a
  // size or alignment the encoding forbids, or no such mechanism (NULL). The
  // stub answers -1 with an empty packet, which is what RSP says for an
  // unsupported operation, and gdb falls back to its software watchpoints.
  int (*gdb_set_watch)(CpuState* cpu, uint64_t addr, uint32_t len, int kind);
  int (*gdb_clear_watch)(CpuState* cpu, int slot);
  // The slots among those installed above that fired on the step just committed
  // (x86: which DR6 B bits the watchpoint set). The stub stops a *free-running*
  // session on this, which it does not do for a guest exception: a watchpoint the
  // debugger asked for is not a fault of the guest's own, and it is reported
  // rather than vectored (cpu/isa/x86/step.c's debug-trap block). NULL = no such
  // report.
  uint32_t (*gdb_watch_hits)(CpuState* cpu);
} isa_ops;

extern const isa_ops k_isa_x86;
extern const isa_ops k_isa_riscv64;

// Every ISA linked into the emulator registers itself here (NULL-terminated).
// Image loading picks an ISA by matching e_machine against elf_machine.
extern const isa_ops* const k_isa_table[];

#endif
