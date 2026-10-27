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
  kDbgVgaIo = 1 << 11,      // guest port writes into the video card
  kDbgPcShot = 1 << 12,     // canonical state row at a linear PC checkpoint
  kDbgPmp = 1 << 13,        // PMP denials: the entry that decided the access
  kDbgVirtio = 1 << 14,     // virtio-mmio queue notify / chain parse / backend
  kDbgAny = 0x7fff,
};

int DebugOn(uint32_t cat);

// Resolved once by DebugInit. The interpreters read these directly on their
// hot paths:
//   g_mem_slow   any category that observes every memory access is on, so the
//                TLB/宿主指针 fast path must be bypassed to keep the event
//                stream identical (mem, bus, and watch=).
//   g_record_raw raw instruction bytes are needed by the trace formats; with
//                none of them on, fetch8 skips the per-byte recording.
//   g_dbg_any    any observation at all is on (mask or watch), so the step's
//                DebugInsn call is worth making.
extern int g_mem_slow;
extern int g_record_raw;
extern int g_dbg_any;

// Parses CEMU_DEBUG into the bitmask, registers watchpoints, and opens the
// tables. Call once before the first step. Inert when the env var is unset.
void DebugInit(void);

// ---- emission points -------------------------------------------------------

// After the PC commit in a step: insn row + state row + regs tick.
void DebugInsn(struct frame* f);

// After the raise consumer installed the resume point: the trap row.
void DebugTrap(struct frame* f);

// In the ISA memory funnels: ld/st + watch rows. addr is linear.
void DebugMem(struct frame* f, uint64_t addr, uint64_t watch_addr, int size, int acc,
              uint64_t val_or_result, int is_load);

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

// PMP denial (kDbgPmp): the physical range, the access class and mode, and
// the lowest-numbered entry that matched (idx = -1: no entry matched).
// A denied access never reaches the bus, so `bus` rows cannot explain it.
void DebugPmpDeny(uint64_t addr, int len, int acc, int mode, int idx, unsigned cfg,
                  uint64_t base, uint64_t size);

// VirtIO transport events (kDbgVirtio): the queue state at notify time, a
// descriptor chain the transport could not parse, and the backend call. The
// guest's used-ring wait is invisible in RAM traces, so these rows are the
// only way to tell "the host never saw the notify" from "the host saw it and
// dropped the chain". QEMU's virtio_mmio trace events are the counterpart.
void DebugVirtio(const char* what, uint64_t a, uint64_t b, uint64_t c, uint64_t d);

// The argument registers of a BIOS/DOS service call, in the hub's plain-integer
// form: the hub knows no ISA's register file (AGENTS.md §七's forbidden
// dependency edges). Plain integers are also why the type exists at all -- the
// parameters are not all in AX. AH=48h takes its size in BX, AH=3Dh its filename
// in DS:DX, AH=0Ah its buffer in ES:DX, so a row carrying only AX could report
// "out of memory" without saying how much was asked for.
//
// The fields carry an `arg_` prefix rather than the register's own name because
// an x86 interpreter spells those as macros (cpu/isa/x86/exec.h: `ax`, `bx`,
// `cx`, `dx`), and a member named `ax` could not be written from there.
typedef struct DbgCallArgs {
  uint32_t arg_ax, arg_bx, arg_cx, arg_dx, arg_ds, arg_es;
} DbgCallArgs;

// A guest's software INT n is a call with a request and an answer, and the two
// halves are separate rows: the request knows the vector (the instruction had it
// in hand), the answer does not (by then the handler is four pops deep). Both
// are keyed by the caller's CS:IP, so the halves line up by address and not by
// bookkeeping -- an interrupt that lands between them cannot shift the pairing.
// AH names the service on the way in (SDM vol.1 5.1 INT n) and every one of
// these services reports failure in CF, which is why the answer row carries it.
//
// QEMU has no row for this: its -d int prints the vector and EAX, but the whole
// block sits inside `if (cr0 & PE)` and fires only on entry
// (target/i386/tcg/seg_helper.c do_interrupt_all), so a real-mode guest's DOS
// conversation has no QEMU-side channel at all. Emitted only for software INT n:
// on an external interrupt or a CPU-raised exception, AX is unrelated to what
// the handler returns.
void DebugGateCall(uint32_t vec, const DbgCallArgs* a, uint32_t caller);
void DebugGateRet(const DbgCallArgs* a, int cf, uint32_t caller);

// Guest port access to the video card's register file (kDbgVgaIo), one row per
// direction. A guest programs the card entirely through these ports, so the
// *sequence* of accesses is what a second model's card has to be compared
// against: the register file read back afterwards says which mode was reached,
// but not which register answered differently on the way there -- and it is the
// reads the guest *decided* on, so a write-only transcript cannot explain a
// guest that then programs the card differently. QEMU offers the same channel to
// its own tests as the vga_std_read_io / vga_std_write_io trace events; without
// this row the two models cannot be lined up access for access.
void DebugVgaIo(uint32_t port, uint32_t val, int is_read);

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

// ---- framebuffer dumps (fbdump=FILE) -----------------------------------------
// dump= shows what the guest left in memory; a rendered picture is not in
// memory at all — it is the display card's own XRGB buffer
// (device/video/display.h), which this hub does not own. The board installs
// these two accessors (the card's picture size, then its pixels) and the item
// is written when the session ends, beside dump=, as the same binary PPM
// (P6) QEMU's `screendump` writes: the two pictures then compare byte for
// byte, which is the headless way to check a renderer (AGENTS.md D17). A
// picture over the 5MB file cap is refused, not truncated, and a run with an
// fbdump must stop by itself (--max-inst, or the guest halting).
typedef int (*DebugFbInfoFn)(void* ctx, int* w, int* h);
typedef const uint32_t* (*DebugFbPixelsFn)(void* ctx);
void DebugSetFbReader(DebugFbInfoFn info, DebugFbPixelsFn pixels, void* ctx);

// ---- the clock frameless rows print ----------------------------------------
// A `mark`/`watch`/`gate` row has no instruction frame, and used to carry no time
// at all: two such rows could not be ordered against the guest's clock, and a
// row seen in one run could not be turned into a checkpoint in the next. The
// board installs its retired-instruction counter here and every frameless row
// prints it in the PC column — the role QEMU's trace stamp
// ("<seq>@<virtual time>") plays in its own log. The point is reproducibility:
// a row at inst=N can be re-run as `--max-inst=N`, and the session end then
// brings the guest's state back through dump=.
void DebugSetClock(const uint64_t* inst_count);

// Halt/exit: the final regs table + session summary (always, once debug is
// active at all).
void DebugSessionEnd(const struct frame* f, const char* stop_reason);

// CEMU_DEBUG grammar this hub implements (AGENTS.md §X):
//   trace[:line|table][=N], state, mem[:ld|st], trap, bus, regs=N,
//   watch=ADDR:SIZE[:r|w|rw] (repeatable), dump=ADDR:SIZE:FILE (repeatable),
//   fbdump=FILE (repeatable, binary PPM of the display card's picture),
//   pcshot=ADDR[:COUNT] (repeatable; state row when the next linear PC is
//   ADDR), budget=N (per-category cap), skip=N (per-category silent window
//   before printing), utf8. Addresses are physical except pcshot's, which is
//   the linear PC the ISA's gdb_pc reports; that is the address a QEMU gdb
//   breakpoint names, so a checkpoint can be compared across the two models
//   without an instruction-count window (which REP loops and timer drift make
//   unusable).

// Session output cap (bytes). All debug writes (table rows, state stream,
// line trace, session summary) are accounted here; once the cap is hit,
// further writes are dropped so a misconfigured CEMU_DEBUG can't fill the
// disk. Returns 1 if n bytes may still be written (and accounts for them),
// 0 once the cap is hit.
int DebugAccountOut(size_t n);

#endif
