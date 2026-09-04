#ifndef CEMU_BOARD_BOARD_H
#define CEMU_BOARD_BOARD_H

#include "bus/bus.h"
#include "cpu/cpu.h"
#include "cpu/step.h"
#include "device/misc/htif.h"
#include "mem/ram.h"

// A board (motherboard) is a device map plus reset state: it decides which
// devices sit at which addresses, how their interrupt lines are wired to the
// CPU, and where execution starts. Which CPU model is installed is not the
// board's business beyond the platform wiring — the ISA rides in isa_ops and
// is picked at image load time.
typedef struct Board {
  const char *name;
  Bus bus;  // memory address space
  Bus io;   // port I/O space; only boards whose CPU has one fill it
  CpuState cpu;
  const isa_ops *isa;
  HtifDevice htif;    // attached only when the loaded image speaks HTIF
  RamDevice *ram;     // main RAM, freed by BoardDestroy
  uint64_t bin_base;  // where the board expects raw images to load
  // Reset state, applied after the image loads (QEMU-style boot flow):
  // 0 = enter at the loaded image entry.
  uint64_t reset_pc;
  // Whether raw binaries of this board speak HTIF (spike does; the QEMU virt
  // and PC contracts do not). ELFs always declare HTIF by symbols.
  int bin_htif;
  // Time-driven device refresh (CLINT MTIP, PIT counters); called by the run
  // loop every step and more often while the CPU sleeps.
  void (*poll)(struct Board *b);
  // Board-specific teardown of non-RAM devices; BoardDestroy calls it after
  // freeing the standard parts.
  void (*destroy)(struct Board *b);
} Board;

// Command-line overridable knobs; 0 means "board default". A board may reject
// values that contradict its layout contract.
typedef struct BoardOpts {
  uint64_t ram_base;
  uint64_t ram_size;
} BoardOpts;

// Creates a board by name ("spike", "x86", "virt"); returns NULL for unknown
// names. The name is the user-facing --machine value.
Board *BoardCreate(const char *name, const BoardOpts *opts);
Board *SpikeBoardCreate(const BoardOpts *opts);
Board *X86BoardCreate(const BoardOpts *opts);
Board *VirtBoardCreate(const BoardOpts *opts);

void BoardRun(Board *b, uint64_t max_inst);
void BoardDestroy(Board *b);

#endif
