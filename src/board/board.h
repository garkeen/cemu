#ifndef CEMU_BOARD_BOARD_H
#define CEMU_BOARD_BOARD_H

#include "bus/bus.h"
#include "cpu/cpu.h"
#include "cpu/step.h"
#include "device/misc/htif.h"
#include "device/video/display.h"
#include "mem/ram.h"

struct GdbStub;    // debug/gdbstub.h; the stub owns run control when attached
struct HostDisplay;  // host/display_win.c; attached by main.c with -display

// A board (motherboard) is a device map plus reset state: it decides which
// devices sit at which addresses, how their interrupt lines are wired to the
// CPU, and where execution starts. Which CPU model is installed is not the
// board's business beyond the platform wiring — the ISA rides in isa_ops and
// is picked at image load time.
typedef struct Board {
  const char* name;
  Bus bus;  // memory address space
  Bus io;   // port I/O space; only boards whose CPU has one fill it
  CpuState cpu;
  const isa_ops* isa;
  HtifDevice htif;    // attached only when the loaded image speaks HTIF
  RamDevice* ram;     // main RAM, freed by BoardDestroy
  uint64_t bin_base;  // where the board expects raw images to load
  // The CPU model this board runs when execution does not come from a loaded
  // image at all — a PC resetting into its firmware ROM. NULL = the board can
  // only run an image, which then names its own ISA.
  const char* default_isa;
  // Reset state, applied after the image loads (QEMU-style boot flow):
  // 0 = enter at the loaded image entry.
  uint64_t reset_pc;
  // Where the CPU starts after a *machine* reset (port 0x92 bit 0, the reset
  // control register at 0xCF9, the keyboard controller's 0xFE command): the
  // firmware's reset vector on a board with a ROM, otherwise the loaded
  // image's entry, which is still in RAM — a reset restarts the machine, not
  // the process. main.c fills this in next to cpu.pc.
  uint64_t entry;
  // A device's reset request is recorded here and acted on at the next
  // instruction boundary: the request arrives from inside a step (the store
  // that wrote the register), and resetting there would pull state out from
  // under the instruction that is still using it.
  int reset_pending;
  // Performs that reset: every device back to its power-on state and the CPU
  // to `entry`. NULL = the machine has no reset facility.
  void (*reset)(struct Board* b);
  // Whether raw binaries of this board speak HTIF (spike does; the QEMU virt
  // and PC contracts do not). ELFs always declare HTIF by symbols.
  int bin_htif;
  // gdb session (-s/-S/-gdb); NULL = plain run. The stub drives the run loop
  // through BoardRunSteps' stop callback — the machine never knows it is
  // being watched.
  struct GdbStub* gdb;
  // Display channel (阶段 3.5 片 2): a board with a video card publishes its
  // rendered framebuffer here; main.c attaches a host window (-display) and
  // the run loop pumps it. display = the attached window, NULL = headless.
  void* display_dev;
  const DisplaySourceOps* display_ops;
  struct HostDisplay* display;
  // Keyboard channel: the board's key sink, fed by the host window's keyboard
  // (on the PC that is the 8042 controller and its IRQ1). NULL = no keyboard.
  void (*key_in)(void* ctx, uint32_t scan, int extended, int up);
  void* key_ctx;
  // Mouse channel: the board's pointer sink, fed by the host window's mouse
  // (on the PC that is the 8042's auxiliary port and its IRQ12). One event
  // carries the movement in mouse counts, the wheel, and the button state
  // afterwards (bit 0 left, bit 1 right, bit 2 middle). NULL = no pointer.
  void (*mouse_in)(void* ctx, int dx, int dy, int dz, int buttons);
  void* mouse_ctx;
  // Serial channel: the board's byte sink, fed by the host's stdin (the console
  // cemu was started from, or a pipe a script drives). On the PC that is COM1's
  // receiver. A board with one takes stdin here and leaves the keyboard to the
  // window — the QEMU `-serial stdio` split. NULL = no serial port.
  void (*serial_in)(void* ctx, int ch);
  void* serial_ctx;
  // Time-driven device refresh (CLINT MTIP, PIT counters); called by the run
  // loop every step and more often while the CPU sleeps.
  void (*poll)(struct Board* b);
  // Idle skip (--skip-idle). While the CPU is halted the run loop normally
  // lets the wait elapse in host time; with this set it instead jumps the
  // emulated clock to the next device deadline (HostTimerWarp), so a guest
  // that sleeps for a tick gets it at once — QEMU's virtual-clock warp. The
  // board names that deadline through next_event_us (microseconds, 0 = no
  // device will fire on its own, so the wait must be host time after all: only
  // the host can produce a key press). NULL = this board has no time-based
  // wakeup source and the flag does nothing.
  int skip_idle;
  int64_t (*next_event_us)(struct Board* b);
  // Board-specific teardown of non-RAM devices; BoardDestroy calls it after
  // freeing the standard parts.
  void (*destroy)(struct Board* b);
} Board;

// Command-line overridable knobs; 0 means "board default". A board may reject
// values that contradict its layout contract.
typedef struct BoardOpts {
  uint64_t ram_base;
  uint64_t ram_size;
  // Firmware ROM image (-bios): the PC board maps it over the top of the first
  // megabyte and resets into it. NULL = no firmware, boot the loaded image.
  const char* bios_path;
  // Disk images for the PC's primary IDE channel (-hda = master, -hdb = slave);
  // NULL = empty bay. Machines without an IDE controller ignore them.
  const char* hda;
  const char* hdb;
  // CD-ROM image for the PC's secondary IDE master (-cdrom): a packet (ATAPI)
  // device with 2048-byte blocks. NULL = empty bay.
  const char* cdrom;
  // --skip-idle: run a halted CPU's idle time at full speed (see Board). Off
  // by default: with it off the emulated clock and the host clock agree, which
  // is what a wall-time-faithful run needs.
  int skip_idle;
} BoardOpts;

// Creates a board by name ("spike", "x86", "virt"); returns NULL for unknown
// names. The name is the user-facing --machine value.
Board* BoardCreate(const char* name, const BoardOpts* opts);
Board* SpikeBoardCreate(const BoardOpts* opts);
Board* X86BoardCreate(const BoardOpts* opts);
Board* VirtBoardCreate(const BoardOpts* opts);

void BoardRun(Board* b, uint64_t max_inst);
// The run loop's inner batch: steps until cpu->halted, the instruction limit,
// or stop_cb returns nonzero (the gdb stub's per-step observation point).
// Returns 1 when a stop callback ended the batch, 0 on halt/limit.
int BoardRunSteps(Board* b, uint64_t max_inst, int (*stop_cb)(void* ctx, CpuState* cpu),
                  void* cb_ctx);
void BoardDestroy(Board* b);

#endif
