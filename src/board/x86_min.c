#include <stdlib.h>
#include <string.h>

#include "board/board.h"
#include "device/char/uart16550.h"
#include "device/input/i8042.h"
#include "device/intc/i8259.h"
#include "device/intc/lapic.h"
#include "device/misc/cmos.h"
#include "device/misc/debug_exit.h"
#include "device/misc/debugcon.h"
#include "device/misc/fwcfg.h"
#include "device/misc/i440fx.h"
#include "device/misc/pci.h"
#include "device/misc/port92.h"
#include "device/timer/i8254.h"
#include "device/video/cga.h"
#include "host/host.h"
#include "util/log.h"

// IBM PC machine (x86_min), the x86 counterpart of spike_min. Real-mode
// memory is 1MB at linear 0; the machine carries 32MB of RAM total
// (kvm-unit-tests images link at 4MB and identity-map through the 4MB-page
// directory). Platform devices: 8259 PIC pair at 0x20/0xA0 (IRQ0..15), 8254
// PIT at 0x40-0x43 (channel 0 -> IRQ0, the periodic timer that wakes hlt),
// COM1 at 0x3F8-0x3FF, the debug-exit device at 0xF4 and fw_cfg at 0x510/0x511
// (QEMU contract) — all in a separate x86 I/O space (CpuState.io) — and the
// Local APIC register page at 0xFEE00000 on the memory bus. The CGA card
// (阶段 3.5 片 2) puts its 16KB frame buffer at 0xB8000 on the memory bus and
// its ports at 0x3D0-0x3DF. Ports and MMIO nobody claims read all-ones and
// drop writes — x86 I/O decode never faults.
//
// Firmware boot (-bios): the top of the first megabyte carries a ROM window
// where the image is mapped, and the CPU resets at the x86 reset vector inside
// it — CS:IP = F000:FFF0 (SDM vol.3 9.1.4). The window is SeaBIOS's own layout:
// its build links the image from 0xc0000 to 0xfffff with the reset vector at
// 0xffff0 (seabios src/config.h BUILD_ROM_START/BUILD_BIOS_ADDR, and the
// generated romlayout32flat.lds), so a 256KiB bios.bin lands byte-for-byte
// where QEMU puts it. The same image is visible at 0xfffc0000, the 256KiB flash
// alias at the end of the 4GiB space that firmware copies itself from (seabios
// src/fw/shadow.c BIOS_SRC_OFFSET; the e820 entry POST reserves for it).
//
// The chipset is the i440FX host bridge (device/misc/i440fx.c): firmware asks
// it — through PCI configuration space — to turn the ROM window into writable
// RAM before it can store its own variables, which is why POST cannot get
// anywhere without it. The keyboard controller (0x60/0x64), System Control
// Port A (0x92) and the RTC/CMOS (0x70/0x71) are here because firmware touches
// them before anything else — A20 is the line the first two drive, and the
// CMOS memory-size registers are how POST learns how much RAM it has.
static const uint64_t kRamSize = 32ULL << 20;
static const uint16_t kPicMasterBase = 0x20;
static const uint16_t kPicSlaveBase = 0xA0;
static const uint16_t kPitBase = 0x40;
static const uint64_t kCom1Base = 0x3F8;
static const uint64_t kCom1Size = 8;
static const uint64_t kDebugExitPort = 0xF4;
static const uint64_t kDebugExitSize = 4;
// The POST log port (QEMU isa-debugcon at its conventional 0x402; seabios
// src/hw/serialio.c DebugOutputPort).
static const uint16_t kDebugConPort = 0x402;
static const uint64_t kPortSpaceSize = 0x10000;
// BIOS ROM window: 256KiB ending at the top of the first megabyte. kRomSize is
// BUILD_BIOS_SIZE plus the two 64KiB option-ROM blocks below it, i.e. exactly
// one bios.bin.
static const uint64_t kRomBase = 0xC0000;
static const uint64_t kRomSize = 0x40000;
// The flash alias at the top of the 4GiB space: same bytes, read-only, and the
// copy source firmware uses while the low window is still ROM (seabios
// shadow.c BIOS_SRC_OFFSET 0xfff00000 is relative to the ROM start).
static const uint64_t kRomAliasBase = 0xFFFC0000;
// The reset vector inside the window: CS:IP = F000:FFF0 is linear 0xFFFF0.
static const uint64_t kResetVector = kRomBase + kRomSize - 0x10;
// Boot-sector handoff (QEMU seabios): BIOS loads the image at 0x7C00 and
// enters it with CS:IP = 0000:7C00, DL = 0x80 (drive number).
static const uint64_t kBootSectorLoad = 0x7C00;
// The host bridge sits at PCI bus 0, device 0, function 0 (QEMU i440fx).
static const uint8_t kHostBridgeDev = 0;

typedef struct X86Board {
  Board base;
  Uart16550 uart;
  DebugExitDevice dexit;
  PicDevice pic;
  PitDevice pit;
  FwCfgDevice fwcfg;
  LapicDevice lapic;
  CgaDevice cga;
  I8042Device kbd;
  Port92Device port92;
  CmosDevice cmos;
  PciBus pci;
  I440fxDevice fx;
  uint8_t* rom;  // the -bios image, mapped in the ROM window (NULL = none)
  uint64_t rom_size;
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

// The ROM window. Reads always answer with the image — the chipset's RAM there
// was initialized from the same bytes, so one buffer serves both states of the
// PAM bits. Stores land only where PAM hands the region to RAM; elsewhere this
// is the ROM chip and the byte is lost (that is the distinction firmware's
// shadowing code depends on, and with PAM reset to ROM it is what sends
// SeaBIOS to the high alias to enable shadowing in the first place).
static uint64_t RomRead(void* dev, uint64_t addr, int size) {
  X86Board* xm = (X86Board*)dev;
  uint64_t off = addr - kRomBase;
  uint64_t v = 0;
  for (int i = 0; i < size; i++)
    v |= (uint64_t)xm->rom[off + (uint64_t)i] << (8 * i);
  return v;
}

static void RomWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  X86Board* xm = (X86Board*)dev;
  if (!I440fxRamWritable(&xm->fx, addr)) return;
  uint64_t off = addr - kRomBase;
  for (int i = 0; i < size; i++)
    xm->rom[off + (uint64_t)i] = (uint8_t)(val >> (8 * i));
}

static const DeviceOps kRomOps = {"bios-rom", RomRead, RomWrite};

// The flash alias: the same image at the top of the 4GiB space, where writes
// are always dropped (no PAM applies there — it is the copy source).
static uint64_t RomAliasRead(void* dev, uint64_t addr, int size) {
  X86Board* xm = (X86Board*)dev;
  uint64_t off = addr - kRomAliasBase;
  uint64_t v = 0;
  for (int i = 0; i < size; i++)
    v |= (uint64_t)xm->rom[off + (uint64_t)i] << (8 * i);
  return v;
}

static void RomAliasWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)dev;
  (void)addr;
  (void)size;
  (void)val;
}

static const DeviceOps kRomAliasOps = {"bios-rom-alias", RomAliasRead, RomAliasWrite};

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

// Chipset -> CPU: A20 is one line, driven by the keyboard controller's output
// port (command 0xd1) and by System Control Port A bit 1.
static void OnA20(void* ctx, int on) {
  CpuState* cpu = (CpuState*)ctx;
  if (cpu->set_a20) cpu->set_a20(cpu, on);
}

// Maps the -bios image into the ROM window. The image sits at the *top* of the
// window (QEMU maps -bios at the end of the BIOS region), so a shorter image
// is zero-padded below and the reset vector still lands at 0xffff0.
static int LoadRom(X86Board* xm, const char* path) {
  HostFile* f = HostFileOpenRead(path);
  if (!f) {
    LogError("cannot open bios image '%s'", path);
    return -1;
  }
  int64_t size = HostFileSize(f);
  if (size <= 0 || (uint64_t)size > kRomSize) {
    HostFileClose(f);
    LogError("bios image '%s' does not fit the 256KiB ROM window", path);
    return -1;
  }
  uint8_t* rom = (uint8_t*)calloc(1, (size_t)kRomSize);
  if (!rom) {
    HostFileClose(f);
    return -1;
  }
  size_t got = HostFileRead(f, rom + (size_t)(kRomSize - (uint64_t)size), (size_t)size);
  HostFileClose(f);
  if (got != (size_t)size) {
    free(rom);
    LogError("short read on bios image '%s'", path);
    return -1;
  }
  xm->rom = rom;
  xm->rom_size = (uint64_t)size;
  return 0;
}

static void X86Destroy(Board* m) {
  X86Board* xm = (X86Board*)m;
  free(xm->rom);
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
  if (opts->bios_path) {
    if (LoadRom(xm, opts->bios_path) != 0) {
      BoardDestroy(m);
      return NULL;
    }
    BusAddRegion(&m->bus, kRomBase, kRomSize, &kRomOps, xm);
    BusAddRegion(&m->bus, kRomAliasBase, kRomSize, &kRomAliasOps, xm);
    m->reset_pc = kResetVector;
    LogInfo("bios '%s': %llu bytes mapped at %llx (+alias %llx), reset at %llx", opts->bios_path,
            (unsigned long long)xm->rom_size, (unsigned long long)kRomBase,
            (unsigned long long)kRomAliasBase, (unsigned long long)m->reset_pc);
  }

  Uart16550* uart = &xm->uart;
  DebugExitDevice* dexit = &xm->dexit;
  PicDevice* pic = &xm->pic;
  PitDevice* pit = &xm->pit;
  FwCfgDevice* fwcfg = &xm->fwcfg;
  LapicDevice* lapic = &xm->lapic;
  CgaDevice* cga = &xm->cga;
  I8042Device* kbd = &xm->kbd;
  Port92Device* p92 = &xm->port92;
  CmosDevice* cmos = &xm->cmos;
  Uart16550Init(uart);
  DebugExitBind(dexit, &m->cpu);
  PicInit(pic);
  PitInit(pit);
  FwCfgInit(fwcfg);
  LapicInit(lapic);
  CgaInit(cga);
  I8042Init(kbd);
  Port92Init(p92);
  CmosInit(cmos);
  PciInit(&xm->pci);
  I440fxInit(&xm->fx, 0, kHostBridgeDev);
  PciAddDevice(&xm->pci, &xm->fx.pci);
  BusAddRegion(&m->io, 0, kPortSpaceSize, &kUnclaimedPortOps, NULL);
  BusAddRegion(&m->io, kCom1Base, kCom1Size, &kUart16550Ops, uart);
  BusAddRegion(&m->io, kDebugExitPort, kDebugExitSize, &kDebugExitOps, dexit);
  DebugConRegister(&m->io, kDebugConPort);
  PicRegister(&m->io, pic, kPicMasterBase, kPicSlaveBase);
  PitRegister(&m->io, pit, kPitBase);
  FwCfgRegister(&m->io, fwcfg);
  I8042Register(&m->io, kbd);
  Port92Register(&m->io, p92);
  CmosRegister(&m->io, cmos);
  CmosSetMemory(cmos, kRamSize);
  PciRegister(&m->io, &xm->pci);
  LapicRegister(&m->bus, lapic);
  CgaRegister(&m->bus, &m->io, cga);

  // Wiring: PIT ch0 -> PIC IRQ0 -> CPU INTR; INTA -> PicAcknowledge; the A20
  // gate comes from the keyboard controller and port 0x92. The hooks live on
  // CpuState (like timer_read/timer_dev), so the machine can install them
  // before the loader picks the ISA.
  PicSetIrqSink(pic, OnPicIrq, &m->cpu);
  PitSetIrqSink(pit, OnPitIrq, pic);
  I8042SetA20Sink(kbd, OnA20, &m->cpu);
  Port92SetA20Sink(p92, OnA20, &m->cpu);
  m->cpu.int_ack = OnIntAck;
  m->cpu.ack_dev = pic;

  m->cpu.halted = kCpuRunning;
  m->cpu.bus = &m->bus;
  m->cpu.io = &m->io;
  m->bin_base = kBootSectorLoad;
  m->default_isa = "x86";  // a BIOS reset has no image to name its ISA
  m->poll = X86Poll;
  m->destroy = X86Destroy;
  m->display_dev = cga;
  m->display_ops = &kCgaDisplayOps;
  return m;
}
