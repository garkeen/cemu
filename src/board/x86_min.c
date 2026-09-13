#include <stdlib.h>

#include "board/board.h"
#include "device/char/uart16550.h"
#include "device/intc/i8259.h"
#include "device/intc/lapic.h"
#include "device/misc/debug_exit.h"
#include "device/misc/fwcfg.h"
#include "device/timer/i8254.h"
#include "device/video/cga.h"
#include "util/log.h"

// IBM PC machine, the x86 counterpart of spike_min. Real-mode memory is 1MB
// at linear 0; the machine carries 32MB of RAM total (kvm-unit-tests images
// link at 4MB and identity-map through the 4MB-page directory). Platform
// devices: 8259 PIC pair at 0x20/0xA0 (IRQ0..15), 8254 PIT at 0x40-0x43
// (channel 0 -> IRQ0, the periodic timer that wakes hlt), COM1 at
// 0x3F8-0x3FF, the debug-exit device at 0xF4 and fw_cfg at 0x510/0x511
// (QEMU contract) — all in a separate x86 I/O space (CpuState.io) — and the
// Local APIC register page at 0xFEE00000 on the memory bus. The CGA card
// (阶段 3.5 片 2) puts its 16KB frame buffer at 0xB8000 on the memory bus and
// its ports at 0x3D0-0x3DF. Ports and MMIO nobody claims read all-ones and
// drop writes — x86 I/O decode never faults.
static const uint64_t kRamSize = 32ULL << 20;
static const uint16_t kPicMasterBase = 0x20;
static const uint16_t kPicSlaveBase = 0xA0;
static const uint16_t kPitBase = 0x40;
static const uint64_t kCom1Base = 0x3F8;
static const uint64_t kCom1Size = 8;
static const uint64_t kDebugExitPort = 0xF4;
static const uint64_t kDebugExitSize = 4;
static const uint64_t kPortSpaceSize = 0x10000;
// Boot-sector handoff (QEMU seabios): BIOS loads the image at 0x7C00 and
// enters it with CS:IP = 0000:7C00, DL = 0x80 (drive number).
static const uint64_t kBootSectorLoad = 0x7C00;

typedef struct X86Board {
  Board base;
  Uart16550 uart;
  DebugExitDevice dexit;
  PicDevice pic;
  PitDevice pit;
  FwCfgDevice fwcfg;
  LapicDevice lapic;
  CgaDevice cga;
} X86Board;

static uint64_t UnclaimedRead(void* dev, uint64_t addr, int size) {
  (void)dev;
  (void)addr;
  return size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
}

static void UnclaimedWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)dev;
  (void)addr;
  (void)size;
  (void)val;
}

static const DeviceOps kUnclaimedPortOps = {"unclaimed-ports", UnclaimedRead, UnclaimedWrite};

// PIC -> CPU: the master's highest-priority deliverable IRQ asserts INTR.
// The board talks to the CpuState hook, not to a CPU-model function, so this
// board carries no ISA header at all.
static void OnPicIrq(void* ctx, int line, int level) {
  CpuState* cpu = (CpuState*)ctx;
  (void)line;  // one INTR line; the vector is fetched on acknowledge
  cpu->set_irq(cpu, 0, level);
}

// INTA cycle: the CPU asks the PIC for the vector number of the pending IRQ.
static int OnIntAck(void* ack_dev) { return PicAcknowledge((PicDevice*)ack_dev); }

// PIT -> PIC: channel edges set/clear IRQ lines (level edges from the PIT
// become edge-triggered IRR bits in the PIC).
static void OnPitIrq(void* ctx, int line, int level) {
  PicDevice* pic = (PicDevice*)ctx;
  PicSetIrq(pic, line, level);
}

static void X86Poll(Board* m) {
  X86Board* xm = (X86Board*)m;
  PitPoll(&xm->pit);
  CgaPoll(&xm->cga);
}

Board* X86BoardCreate(const BoardOpts* opts) {
  if (opts->ram_base || opts->ram_size) {
    LogError("x86 machine has a fixed 1MB real-mode layout");
    return NULL;
  }
  X86Board* xm = (X86Board*)calloc(1, sizeof(X86Board));
  if (!xm) return NULL;
  Board* m = &xm->base;
  m->name = "x86";
  m->ram = RamCreate(0, kRamSize);
  if (!m->ram) {
    free(xm);
    return NULL;
  }
  BusAddRamRegion(&m->bus, 0, kRamSize, &kRamOps, m->ram, m->ram->mem);

  Uart16550* uart = &xm->uart;
  DebugExitDevice* dexit = &xm->dexit;
  PicDevice* pic = &xm->pic;
  PitDevice* pit = &xm->pit;
  FwCfgDevice* fwcfg = &xm->fwcfg;
  LapicDevice* lapic = &xm->lapic;
  CgaDevice* cga = &xm->cga;
  Uart16550Init(uart);
  DebugExitBind(dexit, &m->cpu);
  PicInit(pic);
  PitInit(pit);
  FwCfgInit(fwcfg);
  LapicInit(lapic);
  CgaInit(cga);
  BusAddRegion(&m->io, 0, kPortSpaceSize, &kUnclaimedPortOps, NULL);
  BusAddRegion(&m->io, kCom1Base, kCom1Size, &kUart16550Ops, uart);
  BusAddRegion(&m->io, kDebugExitPort, kDebugExitSize, &kDebugExitOps, dexit);
  PicRegister(&m->io, pic, kPicMasterBase, kPicSlaveBase);
  PitRegister(&m->io, pit, kPitBase);
  FwCfgRegister(&m->io, fwcfg);
  LapicRegister(&m->bus, lapic);
  CgaRegister(&m->bus, &m->io, cga);

  // Wiring: PIT ch0 -> PIC IRQ0 -> CPU INTR; INTA -> PicAcknowledge. The
  // hooks live on CpuState (like timer_read/timer_dev), so the machine can
  // install them before the loader picks the ISA.
  PicSetIrqSink(pic, OnPicIrq, &m->cpu);
  PitSetIrqSink(pit, OnPitIrq, pic);
  m->cpu.int_ack = OnIntAck;
  m->cpu.ack_dev = pic;

  m->cpu.halted = kCpuRunning;
  m->cpu.bus = &m->bus;
  m->cpu.io = &m->io;
  m->bin_base = kBootSectorLoad;
  m->poll = X86Poll;
  m->display_dev = cga;
  m->display_ops = &kCgaDisplayOps;
  return m;
}
