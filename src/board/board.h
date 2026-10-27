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
  // -bios names a raw image to load at bin_base (the virt machine's firmware,
  // QEMU's -bios contract) rather than a ROM the board maps itself (the PC's
  // BIOS window). main.c picks the file path accordingly.
  int bios_image;
  // Where -kernel lands (QEMU's -kernel contract: the firmware's follow-on
  // payload at a fixed board address, e.g. the virt machine's 0x80200000
  // that OpenSBI fw_jump jumps to). 0 = the board takes no -kernel.
  uint64_t kernel_base;
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
  // Absolute pointer channel (all display boards): source-picture pixel
  // coordinates (X right, Y down, already clamped to the picture; the window
  // reports nothing outside the client area). A board with an absolute device
  // (virtio-tablet, Oberon's mouse register) consumes it directly; a
  // relative-only device (PS/2) translates it in the board. mouse_in stays for
  // the debug hub's synthetic relative events.
  void (*mouse_abs)(void* ctx, int x, int y, int dz, int buttons);
  void* mouse_abs_ctx;
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

// -mouse: which pointing device the host's pointer drives. A machine can have
// both a PS/2 mouse and a serial mouse attached, but one host pointer cannot
// sensibly drive both at once — the guest would see every movement twice — so
// the board feeds exactly one. QEMU makes the same choice with its
// `-serial msmouse` being opt-in; here the default is the PS/2 port.
enum { kMousePs2 = 0, kMouseSerial = 1 };

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
  // Floppy image for the PC's A: drive (-fda), served by the 8272 controller
  // over the 8237's channel 2. NULL = no floppy, and firmware then sees no
  // drive at all.
  const char* fda;
  // Disk image for a machine whose storage is a removable card (-dsk): the
  // oberon machine's boot ROM loads the system off it over SPI. NULL = no
  // card, and that machine has nothing to boot from. Machines without such
  // a slot ignore it.
  const char* dsk;
  // Front-panel switches (--switches N) for a machine that has any: bit 0 on
  // the oberon machine makes its boot ROM load over RS232 instead of the
  // card (the reference emulator's -S). Machines without switches ignore it.
  uint32_t switches;
  // Follow-on payload for a firmware-boot machine (-kernel FILE): raw bytes
  // to the board's kernel_base. NULL = none. Boards without a kernel_base
  // ignore it.
  const char* kernel_path;
  // Kernel command line (-append STRING), the QEMU option of the same name:
  // delivered to the firmware through the device tree's /chosen/bootargs.
  // NULL = leave the tree as the board built it.
  const char* append;
  // The display card's own ROM (-vgabios FILE): the int10 video BIOS a PC's
  // display card carries. NULL = no ROM, and the firmware's int10 stub is all
  // a guest finds — DOS-era programs set their video mode through int10, so
  // without it none of them can leave text mode. The machine delivers it to
  // the firmware the way a card without a PCI device does (fw_cfg romfile
  // "vgaroms/vgabios.bin"; device/misc/fwcfg.h).
  const char* vgabios_path;
  // --skip-idle: run a halted CPU's idle time at full speed (see Board). Off
  // by default: with it off the emulated clock and the host clock agree, which
  // is what a wall-time-faithful run needs.
  int skip_idle;
  // Which pointing device the host's pointer feeds (kMousePs2 / kMouseSerial).
  int mouse;
} BoardOpts;

// Creates a board by name ("spike", "x86", "virt"); returns NULL for unknown
// names. The name is the user-facing --machine value.
Board* BoardCreate(const char* name, const BoardOpts* opts);
Board* SpikeBoardCreate(const BoardOpts* opts);
Board* X86BoardCreate(const BoardOpts* opts);
Board* VirtBoardCreate(const BoardOpts* opts);
Board* OberonBoardCreate(const BoardOpts* opts);

void BoardRun(Board* b, uint64_t max_inst);
// The run loop's inner batch: steps until cpu->halted, the instruction limit,
// or stop_cb returns nonzero (the gdb stub's per-step observation point).
// Returns 1 when a stop callback ended the batch, 0 on halt/limit.
int BoardRunSteps(Board* b, uint64_t max_inst, int (*stop_cb)(void* ctx, CpuState* cpu),
                  void* cb_ctx);
void BoardDestroy(Board* b);

#endif
