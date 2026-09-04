#ifndef CEMU_DEBUG_DEBUG_H
#define CEMU_DEBUG_DEBUG_H

// The debug hub (AGENTS.md §X): one event table for every debug output the
// emulator produces. Categories come from CEMU_DEBUG (parsed once at init
// into a bitmask — no getenv on the hot path); emission points are bit
// tests. Observation reads committed state only: interpreters never grow
// debug branches.

#include <stdint.h>

struct frame;

// Category bits (CEMU_DEBUG items).
enum {
  kDbgTraceLine = 1 << 0,   // one-line per-insn format
  kDbgTraceTable = 1 << 1,  // full-column event table
  kDbgState = 1 << 3,       // canonical state stream (diff baseline)
  kDbgMem = 1 << 4,         // ld/st events
  kDbgTrap = 1 << 5,        // exceptions/interrupts
  kDbgBus = 1 << 6,         // MMIO/IO port hits
  kDbgRegs = 1 << 7,        // periodic full register table
  kDbgAny = 0xff,
};

int DebugOn(uint32_t cat);

// Parses CEMU_DEBUG into the bitmask, registers watchpoints, and opens the
// tables. Call once before the first step. Inert when the env var is unset.
void DebugInit(void);

// ---- emission points -------------------------------------------------------

// After the PC commit in a step: insn row + state row + regs tick.
void DebugInsn(struct frame *f);

// After the raise consumer installed the resume point: the trap row.
void DebugTrap(struct frame *f);

// In the ISA memory funnels: ld/st + watch rows. addr is linear.
void DebugMem(struct frame *f, uint64_t addr, int size, int acc,
              uint64_t val_or_result, int is_load);

// MMIO/device hit row: called by the funnels when the winning region has no
// RAM backing. name comes from DeviceOps.
void DebugBus(struct frame *f, const char *dev_name, uint64_t addr, int size,
              int is_load, uint64_t val);

// True when a watchpoint matches (funnels ask before doing the access so
// the row prints even when the access itself faults).
int DebugWatchHit(uint64_t addr, int size, int is_load);

// Halt/exit: the final regs table + session summary (always, once debug is
// active at all).
void DebugSessionEnd(const struct frame *f, const char *stop_reason);

// CEMU_DEBUG grammar this hub implements (AGENTS.md §X):
//   trace[:line|table][=N], state, mem[:ld|st], trap, bus, regs=N,
//   watch=ADDR:SIZE[:r|w|rw] (repeatable), budget=N (per-category cap),
//   skip=N (per-category silent window before printing), utf8.

#endif
