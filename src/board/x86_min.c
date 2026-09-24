#include <stdlib.h>
#include <string.h>

#include "board/board.h"
#include "device/char/uart16550.h"
#include "device/input/i8042.h"
#include "device/intc/i8259.h"
#include "device/intc/lapic.h"
#include "device/intc/ioapic.h"
#include "debug/debug.h"
#include "device/misc/cmos.h"
#include "device/misc/debug_exit.h"
#include "device/misc/debugcon.h"
#include "device/misc/fwcfg.h"
#include "device/misc/i440fx.h"
#include "device/misc/pci.h"
#include "device/misc/piix3.h"
#include "device/misc/port92.h"
#include "device/misc/testdev.h"
#include "device/storage/ide.h"
#include "device/timer/i8254.h"
#include "device/video/cga.h"
#include "host/host.h"
#include "util/log.h"

// IBM PC machine (x86_min), the x86 counterpart of spike_min. Real-mode
// memory is 1MB at linear 0; the machine carries 32MB of RAM by default
// (kDefaultRamSize, overridable with --mem; kvm-unit-tests images link at 4MB
// and identity-map through the 4MB-page directory, xv6's kernel assumes at
// least its own 224MB PHYSTOP). Platform devices: 8259 PIC pair at 0x20/0xA0
// (IRQ0..15), 8254 PIT at 0x40-0x43 (channel 0 -> IRQ0, the periodic timer that
// wakes hlt), COM1 at 0x3F8-0x3FF, the debug-exit device at 0xF4, fw_cfg at
// 0x510/0x511 (QEMU contract) and the kvm-unit-tests IRQ injection window at
// 0x2000-0x200F (device/misc/testdev.c) — all in a separate x86 I/O space
// (CpuState.io) —
// and the Local APIC register page at 0xFEE00000 on the memory bus. The CGA card
// (阶段 3.5 片 2) puts its 16KB frame buffer at 0xB8000 on the memory bus and
// its ports at 0x3D0-0x3DF. Ports and MMIO nobody claims read all-ones and
// drop writes — x86 I/O decode never faults — and an address no memory region
// covers answers the same way, the open-bus value of a PC whose decoders all
// decline the cycle.
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
// The chipset is the i440FX host bridge (device/misc/i440fx.c) plus the PIIX3
// PCI-to-ISA bridge and its IDE function (device/misc/piix3.c,
// device/storage/ide.h). Firmware asks the host bridge — through PCI
// configuration space — to turn the ROM window into writable RAM before it can
// store its own variables, which is why POST cannot get anywhere without it.
// The keyboard controller (0x60/0x64), System Control Port A (0x92) and the
// RTC/CMOS (0x70/0x71) are here because firmware touches them before anything
// else — A20 is the line the first two drive, and the CMOS memory-size
// registers are how POST learns how much RAM it has. The IDE controller is how
// the machine has a disk at all: firmware reads the boot sector off it with
// int13h, and an operating system reads the rest of its medium through the same
// task file.
static const uint64_t kDefaultRamSize = 32ULL << 20;
// The chipset's MMIO windows start at the Local APIC page
// (device/intc/lapic.c) and continue up through the PCI/BIOS holes at the top
// of the 4GiB space, so RAM can only live below them.
static const uint64_t kRamLimit = 0xFEE00000ULL;
static const uint64_t kMinRamSize = 1ULL << 20;
static const uint64_t kAddressSpace = 1ULL << 32;
static const uint16_t kPicMasterBase = 0x20;
static const uint16_t kPicSlaveBase = 0xA0;
static const uint16_t kPitBase = 0x40;
static const uint64_t kCom1Base = 0x3F8;
static const uint64_t kCom1Size = 8;
// COM1's interrupt line. The on-board serial port's IRQ is fixed by the PC/AT
// wiring (COM1 -> IRQ4, COM2 -> IRQ3; PC/AT Technical Reference), and the PIIX3
// routes it to both controllers like every other ISA line.
static const int kCom1Irq = 4;
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
// The host bridge sits at PCI bus 0, device 0, function 0, and the PIIX3 at
// device 1: function 0 the ISA bridge, function 1 the IDE controller (i440FX
// and PIIX3 datasheets; QEMU hw/pci-host/i440fx.c and hw/isa/piix3.c).
static const uint8_t kHostBridgeDev = 0;
static const uint8_t kPiixDev = 1;
static const uint8_t kPciBus0 = 0;

// Every ISA IRQ wire in a PC/AT reaches two controllers — the 8259 pair and
// the I/O APIC's pins 0..15 — and the guest's programming decides which one
// delivers.
typedef struct IrqBus {
  PicDevice* pic;
  IoapicDevice* ioapic;
} IrqBus;

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
  TestDevDevice testdev;
  PciBus pci;
  I440fxDevice fx;
  IoapicDevice ioapic;
  Piix3BridgeDevice piix;
  IdeDevice ide;
  // INTR is one line and either controller can assert it (UpdateIntr).
  int pic_irq;
  int lapic_irq;
  int intr_level;  // the combined pin level last driven (kDbgMark edge detect)
  IrqBus irqbus;  // the ISA IRQ fan-out: the 8259s on one side, the I/O APIC on the other
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
static const DeviceOps kUnclaimedMemOps = {"unclaimed-mem", UnclaimedRead, UnclaimedWrite};

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

// The CPU's INTR line: either controller drives it — the PIC while the machine
// runs on the 8259s (its firmware), the LAPIC once a guest enables the APIC —
// and the board owns the combined level, the way a single INTR pin behaves.
static void UpdateIntr(X86Board* xm) {
  CpuState* cpu = &xm->base.cpu;
  int level = xm->pic_irq || xm->lapic_irq;
  // The CPU's INTR pin is otherwise invisible from outside: the ISA fan-out
  // mark shows what reached the controllers, this one shows what left them
  // (kDbgMark). Without it a guest that never handles its timer cannot be
  // told apart from a controller that never asserted.
  if (level != xm->intr_level) {
    xm->intr_level = level;
    if (DebugOn(kDbgMark)) DebugMark("intr", xm->pic_irq, xm->lapic_irq);
  }
  cpu->set_irq(cpu, 0, level);
}

static void OnPicIrq(void* ctx, int line, int level) {
  X86Board* xm = (X86Board*)ctx;
  (void)line;  // one INTR line; the vector is fetched on acknowledge
  xm->pic_irq = level;
  UpdateIntr(xm);
}

static void OnLapicIrq(void* ctx, int line, int level) {
  X86Board* xm = (X86Board*)ctx;
  (void)line;
  xm->lapic_irq = level;
  UpdateIntr(xm);
}

// The I/O APIC hands a redirection entry's vector to the destination APIC: a
// physical-mode destination names its ID, a logical-mode one names its
// flat-model mask (SDM vol.3 11.5.3; 82093AA §3.2.4 destination format).
static void OnIoapicDeliver(void* ctx, int dest, int logical, int vector, int trigger) {
  X86Board* xm = (X86Board*)ctx;
  int match = logical ? (dest & LapicLogicalMask(&xm->lapic)) : (dest == LapicId(&xm->lapic));
  // A redirection entry that names a destination nobody answers is a silently
  // dropped interrupt; the mark reports the destination and whether it matched
  // (kDbgMark, negative vector = no match).
  if (DebugOn(kDbgMark)) DebugMark("ioapic", dest, match ? vector : -vector);
  if (match) LapicDeliver(&xm->lapic, vector, trigger);
}

// An end of interrupt retires the I/O APIC pin whose vector was in service.
static void OnLapicEoi(void* ctx, int vector) {
  X86Board* xm = (X86Board*)ctx;
  IoapicEoi(&xm->ioapic, vector);
}

// INTA: the processor asks whichever controller asserted INTR. The LAPIC
// answers first when a guest enabled it, and the 8259 answers otherwise —
// QEMU's cpu_get_pic_interrupt splits it the same way.
static int OnIntAck(void* ctx) {
  X86Board* xm = (X86Board*)ctx;
  int vec = LapicAcknowledge(&xm->lapic);
  if (vec < 0) vec = PicAcknowledge(&xm->pic);
  // The acknowledge is the point of no return for a request: the controller
  // marks it in service and only the handler's EOI retires it (kDbgMark).
  if (DebugOn(kDbgMark)) DebugMark("inta", vec, xm->pic_irq);
  return vec;
}

// An ISA device's interrupt line into the PIC: the PIT on IRQ0, the IDE
// channels on the fixed IRQ 14/15 (the PIIX3's compatibility-mode wiring).
// An ISA device's interrupt line: the PIT on IRQ0, the IDE channels on the
// fixed IRQ 14/15 (the PIIX3's compatibility-mode wiring). The wire reaches
// both controllers, so each of them can be the one that delivers.
static void OnIsaIrq(void* ctx, int line, int level) {
  IrqBus* b = (IrqBus*)ctx;
  PicSetIrq(b->pic, line, level);
  IoapicSetPin(b->ioapic, line, level);
  // Line transitions into the controllers (and from there to the CPU): the ISA
  // fan-out is otherwise invisible from outside (kDbgMark).
  if (DebugOn(kDbgMark)) DebugMark("isa", line, level);
}

// Host keys -> the 8042: the keyboard's byte (0xe0 first for an extended key,
// bit 7 on a release) lands in the controller's output queue, which raises
// IRQ1 while a byte waits; the guest's keyboard driver reads it from 0x60.
static void OnHostKey(void* ctx, uint32_t scan, int extended, int up) {
  X86Board* xm = (X86Board*)ctx;
  if (scan == 0 || scan > 0x7f) return;  // Win32 sends no scan code for a few keys
  if (extended) I8042KeyByte(&xm->kbd, 0xe0);
  if (DebugOn(kDbgMark)) DebugMark("hostkey", (int)scan, up);
  I8042KeyByte(&xm->kbd, (uint8_t)(scan | (up ? 0x80u : 0u)));
}

// Host input -> COM1's receiver: the byte arrives at the UART exactly as a
// character on the SIN pin would, so the guest reads it through the same
// RBR/FIFO/IIR path and can be woken by IRQ4.
static void OnHostSerial(void* ctx, int ch) {
  X86Board* xm = (X86Board*)ctx;
  if (DebugOn(kDbgMark)) DebugMark("hostserial", ch, 0);
  Uart16550Receive(&xm->uart, ch);
}

// COM1's interrupt line into the ISA fan-out (the UART's own source number is
// not the IRQ: the board owns the wiring).
static void OnCom1Irq(void* ctx, int src, int level) {
  (void)src;
  OnIsaIrq(ctx, kCom1Irq, level);
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
  IdeDestroy(&xm->ide);
}

static void X86Poll(Board* m) {
  X86Board* xm = (X86Board*)m;
  PitPoll(&xm->pit);
  CgaPoll(&xm->cga);
  // The APIC timer is the machine's clock: xv6 preempts on it, and its count
  // is derived from the host clock rather than from steps.
  LapicPoll(&xm->lapic);
  // COM1's character timeout is the same kind of source: the chip decides on
  // its own that a partial receive FIFO has waited long enough.
  Uart16550Poll(&xm->uart);
  // The RTC's periodic rate and its once-a-second update/alarm comparison are
  // wall-clock sources too (PitPoll's contract).
  CmosPoll(&xm->cmos);
}

// The next moment a device wakes the processor on its own: the PC's two
// time-driven sources are the PIT (IRQ0) and the APIC timer. The run loop's
// --skip-idle jumps a halted CPU to whichever comes first.
static int64_t X86NextEventUs(Board* m) {
  X86Board* xm = (X86Board*)m;
  int64_t pit = PitNextEventUs(&xm->pit);
  int64_t apic = LapicNextEventUs(&xm->lapic);
  if (pit <= 0) return apic;
  if (apic <= 0) return pit;
  return pit < apic ? pit : apic;
}

// A device asked for a machine reset (D18). The request is recorded, not acted
// on here: it arrives from inside the step that wrote the register, and the
// reset happens at the next instruction boundary (run.c), where no instruction
// is still using the state it would tear down.
static void RequestReset(void* ctx) {
  X86Board* xm = (X86Board*)ctx;
  xm->base.reset_pending = 1;
  if (DebugOn(kDbgMark)) DebugMark("reset-req", 0, 0);
}

// The machine's wiring: which sink each device's line reaches, and which
// devices may ask for a reset. Every Init below clears its own sinks, so this
// runs again after a reset — it is the state of the connections, not their
// layout (the bus registration in X86BoardCreate happens once).
static void WireDevices(X86Board* xm) {
  Board* m = &xm->base;
  xm->irqbus.pic = &xm->pic;
  xm->irqbus.ioapic = &xm->ioapic;
  PicSetIrqSink(&xm->pic, OnPicIrq, xm);
  LapicSetIrqSink(&xm->lapic, OnLapicIrq, xm);
  LapicSetEoiSink(&xm->lapic, OnLapicEoi, xm);
  PitSetIrqSink(&xm->pit, OnIsaIrq, &xm->irqbus);
  I8042SetIrqSink(&xm->kbd, OnIsaIrq, &xm->irqbus);
  IdeSetIrqSink(&xm->ide, OnIsaIrq, &xm->irqbus);
  Uart16550SetIrqSink(&xm->uart, OnCom1Irq, &xm->irqbus);
  CmosSetIrqSink(&xm->cmos, OnIsaIrq, &xm->irqbus);
  // The kvm-unit-tests injection window drives the same ISA wires the devices
  // do, so it reaches both controllers through the same fan-out.
  TestDevSetIrqSink(&xm->testdev, OnIsaIrq, &xm->irqbus);
  IoapicSetDeliverSink(&xm->ioapic, OnIoapicDeliver, xm);
  I8042SetA20Sink(&xm->kbd, OnA20, &m->cpu);
  Port92SetA20Sink(&xm->port92, OnA20, &m->cpu);
  // The three ways a guest can reboot the machine on a PC (D18): port 0x92's
  // INIT_NOW line, the PIIX3 reset control register at 0xCF9, and the keyboard
  // controller's 0xFE command.
  Port92SetResetSink(&xm->port92, RequestReset, xm);
  Piix3SetResetSink(&xm->piix, RequestReset, xm);
  I8042SetResetSink(&xm->kbd, RequestReset, xm);
  m->cpu.int_ack = OnIntAck;
  m->cpu.ack_dev = xm;
}

// The machine's reset. Devices first, so the firmware that starts at the reset
// vector finds a power-on machine, and the CPU last, because its reset state
// decides where the first fetch goes. The Inits are the ones the board came up
// with — they are written to be re-runnable — with two exceptions, because a
// reset is not a power cycle: CMOS keeps its battery-backed contents, and the
// IDE controller keeps its media and the configuration the chipset was
// programmed with. The layout (bus registration, port windows, the images) is
// never touched.
static void X86Reset(Board* m) {
  X86Board* xm = (X86Board*)m;
  Uart16550Init(&xm->uart);
  PicInit(&xm->pic);
  PitInit(&xm->pit);
  FwCfgInit(&xm->fwcfg);
  LapicInit(&xm->lapic);
  IoapicInit(&xm->ioapic);
  CgaInit(&xm->cga);
  I8042Init(&xm->kbd);
  Port92Init(&xm->port92);
  TestDevInit(&xm->testdev);
  CmosReset(&xm->cmos);
  // The PCI bus forgets its device list on reset (the devices keep their
  // configuration), so the three functions are re-enumerated here.
  PciInit(&xm->pci);
  PciAddDevice(&xm->pci, &xm->fx.pci);
  PciAddDevice(&xm->pci, &xm->piix.pci);
  PciAddDevice(&xm->pci, &xm->ide.pci);
  IdeReset(&xm->ide);
  DebugExitBind(&xm->dexit, &m->cpu);
  WireDevices(xm);
  m->cpu.pc = m->entry;
  m->cpu.halted = kCpuRunning;
  m->cpu.wait = 0;
  m->cpu.exit_code = 0;
  m->isa->init(&m->cpu);
  LogInfo("machine reset: entry=%llx", (unsigned long long)m->entry);
}

Board* X86BoardCreate(const BoardOpts* opts) {
  if (opts->ram_base) {
    LogError("x86 machine places RAM at linear 0");
    return NULL;
  }
  uint64_t ram_size = opts->ram_size ? opts->ram_size : kDefaultRamSize;
  if (ram_size < kMinRamSize || ram_size > kRamLimit) {
    LogError("x86 machine takes between %lluMB and %lluMB of RAM",
             (unsigned long long)(kMinRamSize >> 20), (unsigned long long)(kRamLimit >> 20));
    return NULL;
  }
  X86Board* xm = (X86Board*)calloc(1, sizeof(X86Board));
  if (!xm) return NULL;
  Board* m = &xm->base;
  m->name = "x86";
  m->ram = RamCreate(0, ram_size);
  if (!m->ram) {
    free(xm);
    return NULL;
  }
  // The open bus first: every address the chipset's decoders do not claim
  // answers with all ones and swallows writes (the RAM region below is smaller,
  // so it wins wherever it applies).
  BusAddRegion(&m->bus, 0, kAddressSpace, &kUnclaimedMemOps, NULL);
  BusAddRamRegion(&m->bus, 0, ram_size, &kRamOps, m->ram, m->ram->mem);
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
  IdeDevice* ide = &xm->ide;
  Uart16550Init(uart);
  DebugExitBind(dexit, &m->cpu);
  PicInit(pic);
  PitInit(pit);
  FwCfgInit(fwcfg);
  LapicInit(lapic);
  IoapicInit(&xm->ioapic);
  CgaInit(cga);
  I8042Init(kbd);
  Port92Init(p92);
  CmosInit(cmos);
  TestDevInit(&xm->testdev);
  PciInit(&xm->pci);
  I440fxInit(&xm->fx, kPciBus0, kHostBridgeDev);
  Piix3BridgeInit(&xm->piix, kPciBus0, kPiixDev);
  IdeInit(ide, kPciBus0, kPiixDev);
  if (opts->hda && IdeAttach(ide, 0, 0, opts->hda, kIdeMediaDisk) != 0) {
    LogError("cannot attach -hda image '%s'", opts->hda);
    BoardDestroy(m);
    return NULL;
  }
  if (opts->hdb && IdeAttach(ide, 0, 1, opts->hdb, kIdeMediaDisk) != 0) {
    LogError("cannot attach -hdb image '%s'", opts->hdb);
    BoardDestroy(m);
    return NULL;
  }
  // The CD-ROM sits on the secondary master, which is where a PC puts it (and
  // where QEMU's -cdrom lands by default).
  if (opts->cdrom && IdeAttach(ide, 1, 0, opts->cdrom, kIdeMediaCd) != 0) {
    LogError("cannot attach -cdrom image '%s'", opts->cdrom);
    BoardDestroy(m);
    return NULL;
  }
  PciAddDevice(&xm->pci, &xm->fx.pci);
  PciAddDevice(&xm->pci, &xm->piix.pci);
  PciAddDevice(&xm->pci, &ide->pci);
  BusAddRegion(&m->io, 0, kPortSpaceSize, &kUnclaimedPortOps, NULL);
  BusAddRegion(&m->io, kCom1Base, kCom1Size, &kUart16550Ops, uart);
  BusAddRegion(&m->io, kDebugExitPort, kDebugExitSize, &kDebugExitOps, dexit);
  DebugConRegister(&m->io, kDebugConPort);
  PicRegister(&m->io, pic, kPicMasterBase, kPicSlaveBase);
  PicRegisterElcr(&m->io, pic);
  PitRegister(&m->io, pit, kPitBase);
  FwCfgRegister(&m->io, fwcfg);
  I8042Register(&m->io, kbd);
  Port92Register(&m->io, p92);
  CmosRegister(&m->io, cmos);
  TestDevRegister(&m->io, &xm->testdev);
  CmosSetMemory(cmos, ram_size);
  PciRegister(&m->io, &xm->pci);
  Piix3Register(&m->io, &xm->piix);
  IdeRegister(&m->io, ide);
  // The bus-master engine moves a transfer through system memory (the PRD table
  // and the host's buffers), so the controller needs the address space the task
  // file never touches.
  IdeSetDmaBus(ide, &m->bus);
  LapicRegister(&m->bus, lapic);
  IoapicRegister(&m->bus, &xm->ioapic);
  CgaRegister(&m->bus, &m->io, cga);

  // Wiring, the way a PC/AT is wired: every ISA IRQ line reaches both
  // controllers (the 8259 pair and the I/O APIC's pins), the LAPIC is the
  // processor's own interrupt source, and the A20 gate comes from the keyboard
  // controller and port 0x92. The hooks live on CpuState (like
  // timer_read/timer_dev), so the machine installs them before the loader
  // picks the ISA.
  WireDevices(xm);
  // Keys from the host window enter the machine at the 8042 (IRQ1); stdin
  // enters it at COM1's receiver, which is where a terminal belongs on a PC.
  m->key_in = OnHostKey;
  m->key_ctx = xm;
  m->serial_in = OnHostSerial;
  m->serial_ctx = xm;

  m->cpu.halted = kCpuRunning;
  m->cpu.bus = &m->bus;
  m->cpu.io = &m->io;
  m->bin_base = kBootSectorLoad;
  m->default_isa = "x86";  // a BIOS reset has no image to name its ISA
  m->poll = X86Poll;
  m->next_event_us = X86NextEventUs;
  m->skip_idle = opts->skip_idle;
  m->reset = X86Reset;  // port 0x92 bit 0 / RCR 0xCF9 / i8042 0xFE (D18)
  m->destroy = X86Destroy;
  m->display_dev = cga;
  m->display_ops = &kCgaDisplayOps;
  return m;
}
