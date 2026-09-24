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
  kDbgGdb = 1 << 8,         // gdb stub protocol packets (tx/rx)
  kDbgMark = 1 << 9,        // frameless notes: irq lines, host input
  kDbgScreen = 1 << 10,     // text-screen mirror rows (console with no serial)
  kDbgAny = 0x7ff,
};

int DebugOn(uint32_t cat);

// Parses CEMU_DEBUG into the bitmask, registers watchpoints, and opens the
// tables. Call once before the first step. Inert when the env var is unset.
void DebugInit(void);

// ---- emission points -------------------------------------------------------

// After the PC commit in a step: insn row + state row + regs tick.
void DebugInsn(struct frame* f);

// After the raise consumer installed the resume point: the trap row.
void DebugTrap(struct frame* f);

// In the ISA memory funnels: ld/st + watch rows. addr is linear.
void DebugMem(struct frame* f, uint64_t addr, int size, int acc, uint64_t val_or_result,
              int is_load);

// MMIO/device hit row: called by the funnels when the winning region has no
// RAM backing. name comes from DeviceOps.
void DebugBus(struct frame* f, const char* dev_name, uint64_t addr, int size, int is_load,
              uint64_t val);

// gdb stub packet row (stage 3.5): the stub's tx/rx protocol traffic.
void DebugGdbPkt(int is_tx, const char* pkt);

// gdb stub lifecycle row (same category): accept/detach/session/stop-reason
// transitions — the state machine around the packets.
void DebugGdbNote(const char* note);

// Frameless note row (kDbgMark): board-level wiring and device-side events —
// interrupt-line transitions, host input — have no instruction frame to report.
// a/b are the two numbers the call site wants in the detail cell.
void DebugMark(const char* what, int a, int b);

// ---- synthetic host input (mouse=, key=) -----------------------------------
// A guest-driven test can reach everything the guest can program, but not the
// host event path itself: a probe running as a guest cannot move a pointer or
// press a key. These items feed the machine's input sinks exactly the way the
// display window does, which is what makes the host-input path observable from
// a headless run — the same role QEMU's monitor `mouse_move`/`sendkey` plays
// for its tests.
//
//   mouse=DX:DY:BUTTONS[:WHEEL]@N
//   key=SCAN[:EXT[:UP]]@N
//
// DX/DY are the movement in mouse counts and WHEEL the wheel movement (signed
// C literals — hex needs 0x); BUTTONS is the button state after the event
// (bit 0 left, bit 1 right, bit 2 middle). SCAN is the set-1 make code the host
// reports, EXT marks the 0xe0-prefixed keys and UP a release — the triple
// HostDisplaySetKeySink hands out, so `key=0x1e@N,key=0x9e@N` is a press and
// its release. @N is the period in instructions: the event repeats every N
// instructions, so a guest that enables the device whenever it likes still sees
// one. An item without @N, or with too few fields, is an error, not a silent
// drop.
typedef enum { kInjMouse = 1, kInjKey } DebugInjectionKind;

typedef struct DebugInjection {
  int kind;
  int dx, dy, dz, buttons;  // kInjMouse
  uint32_t scan;            // kInjKey
  int extended, up;         // kInjKey
} DebugInjection;

// Fills `out` with one event due at instruction `inst_count` and returns 1;
// returns 0 when none is due. Call in a loop until it returns 0.
int DebugNextInjection(uint64_t inst_count, DebugInjection* out);

// Frameless text row (kDbgScreen): the guest console as read back from the
// video device, for guests that print only to VRAM.
void DebugText(const char* kind, const char* text);

// True when a watchpoint matches (funnels ask before doing the access so
// the row prints even when the access itself faults).
int DebugWatchHit(uint64_t addr, int size, int is_load);

// ---- guest-memory dumps (dump=ADDR:SIZE:FILE) -------------------------------
// A `watch` row says *that* an address was touched; it cannot say what the
// guest left in a structure nobody reads again — a stopped guest's log buffer,
// page tables, task structs. Reading those needs the address space, which the
// hub does not own (bus/ stays outside debug/), so the board installs a
// physical-memory reader and the dump items are written when the session ends.
// A dump run must therefore stop by itself (--max-inst, or the guest halting):
// a host signal skips the session end and writes nothing.
typedef int (*DebugMemReadFn)(void* ctx, uint64_t addr, uint8_t* buf, int len);
void DebugSetMemReader(DebugMemReadFn read, void* ctx);

// Halt/exit: the final regs table + session summary (always, once debug is
// active at all).
void DebugSessionEnd(const struct frame* f, const char* stop_reason);

// CEMU_DEBUG grammar this hub implements (AGENTS.md §X):
//   trace[:line|table][=N], state, mem[:ld|st], trap, bus, regs=N,
//   watch=ADDR:SIZE[:r|w|rw] (repeatable), dump=ADDR:SIZE:FILE (repeatable),
//   budget=N (per-category cap), skip=N (per-category silent window before
//   printing), utf8. Addresses are physical.

// Session output cap (bytes). All debug writes (table rows, state stream,
// line trace, session summary) are accounted here; once the cap is hit,
// further writes are dropped so a misconfigured CEMU_DEBUG can't fill the
// disk. Returns 1 if n bytes may still be written (and accounts for them),
// 0 once the cap is hit.
int DebugAccountOut(size_t n);

#endif
