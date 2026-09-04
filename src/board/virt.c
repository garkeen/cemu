#include <stdlib.h>
#include <string.h>
#include "board/board.h"
#include "virt_dtb.h"
#include "device/timer/clint.h"
#include "device/intc/plic.h"
#include "device/char/uart16550.h"
#include "device/misc/sifive_test.h"
// The only CPU-model header this board needs: the interrupt line ids the
// CLINT and PLIC drive. Everything else about the CPU is reached through the
// CpuState hooks, so the board never names a model function.
#include "cpu/isa/riscv64/platform.h"
#include "util/log.h"

// QEMU virt machine, calibrated from the real thing:
// `qemu-system-riscv64 -machine virt,dumpdtb` for the layout below, and a
// monitor `pmemsave` of the mask ROM for the reset vector and fw_dynamic
// info. Layout constants follow hw/riscv/virt.c memory map.
static const uint64_t kDramBase = 0x80000000ULL;
static const uint64_t kDramSizeDefault = 128ULL << 20;
static const uint64_t kMromBase = 0x1000ULL;        // mask ROM: reset vector
static const uint64_t kMromSize = 0x1000ULL;
static const uint64_t kTestBase = 0x100000ULL;      // sifive,test1 finisher
static const uint64_t kTestSize = 0x1000ULL;
static const uint64_t kClintBase = 0x02000000ULL;   // sifive,clint0
static const uint64_t kClintSize = 0x10000ULL;
static const uint64_t kPlicBase = 0x0c000000ULL;    // sifive,plic-1.0.0
static const uint64_t kPlicSize = 0x600000ULL;
static const uint64_t kUartBase = 0x10000000ULL;    // ns16550a serial0
static const uint64_t kUartSize = 0x100ULL;
static const uint64_t kTimebaseHz = 10000000;       // DTB timebase-frequency
// The FDT sits 2 MiB below the top of DRAM, 2 MiB aligned (QEMU
// hw/riscv/boot.c riscv_compute_fdt_addr; measured 0x87e00000 at 128 MiB).
static const uint64_t kFdtReserve = 2ULL << 20;

// UART0 is PLIC source 10 (DTB /soc/serial@10000000 interrupts).
enum { kVirtUartIrq = 10 };

// Reset vector, word-for-word from the QEMU mask ROM dump. The six
// instructions load a0 = mhartid, a1 = fdt address (0x1020), a2 =
// fw_dynamic_info (0x1028) and jump to the firmware entry (0x1018).
static const uint32_t kResetVec[6] = {
    0x00000297,  // auipc t0, 0
    0x02828613,  // addi a2, t0, 40
    0xf1402573,  // csrr a0, mhartid
    0x0202b583,  // ld a1, 32(t0)
    0x0182b283,  // ld t0, 24(t0)
    0x00028067,  // jr t0
};
// fw_dynamic_info fields (opensbi include/sbi/fw_dynamic.h); the values are
// QEMU's no-kernel configuration, measured from the same dump.
static const uint64_t kFwDynInfo[6] = {
    0x4942534f,  // magic FW_DYNAMIC_INFO_MAGIC_VALUE
    2,           // version
    0,           // next_addr: no -kernel payload
    1,           // next_mode: S-mode (FW_DYNAMIC_INFO_NEXT_MODE_S)
    0,           // options
    0,           // boot_hart
};

typedef struct VirtBoard {
  Board base;
  ClintDevice clint;
  PlicDevice plic;
  Uart16550 uart;
  SifiveTestDevice test;
  RamDevice *mrom;
} VirtBoard;

// ---- wiring: device lines to CPU interrupt inputs ----

static void OnClintIrq(void *ctx, int line, int level) {
  CpuState *cpu = (CpuState *)ctx;
  cpu->set_irq(cpu, line == kClintLineMsip ? kIrqMsip : kIrqMtip, level);
}

static void OnPlicIrq(void *ctx, int line, int level) {
  CpuState *cpu = (CpuState *)ctx;
  // PLIC context 0 is hart0 M-mode, context 1 is hart0 S-mode (DTB
  // interrupts-extended: M ext then S ext).
  cpu->set_irq(cpu, line == 0 ? kIrqMeip : kIrqSeip, level);
}

static void OnUartIrq(void *ctx, int src, int level) {
  (void)src;
  PlicDeviceIrq((PlicDevice *)ctx, kVirtUartIrq, level);
}

static uint64_t ReadTime(void *dev) { return ClintMtime((ClintDevice *)dev); }

static void VirtPoll(Board *m) {
  VirtBoard *vm = (VirtBoard *)m;
  ClintPoll(&vm->clint);
}

// BoardDestroy frees the Board itself; this hook releases the extra
// allocations only.
static void VirtDestroy(Board *m) {
  VirtBoard *vm = (VirtBoard *)m;
  RamDestroy(vm->mrom);
}

// ---- FDT ----

static uint32_t Be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}

static void PutBe64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i));
}

// Walks the FDT structure (devicetree specification 0.4, big-endian) and
// rewrites the size cells of the memory node's reg property to the actual
// RAM size, so the tree QEMU generated for 128 MiB describes our DRAM.
static void PatchDtbMemorySize(uint8_t *dtb, uint64_t ram_size) {
  size_t pos = Be32(dtb + 8);   // off_dt_struct
  size_t strings = Be32(dtb + 12);
  int depth = 0;
  int in_memory = 0;
  enum { kFdtBegin = 1, kFdtEndNode = 2, kFdtProp = 3, kFdtNop = 4, kFdtEnd = 9 };
  for (;;) {
    uint32_t tok = Be32(dtb + pos);
    pos += 4;
    if (tok == kFdtBegin) {
      const char *name = (const char *)(dtb + pos);
      size_t nlen = strlen(name);
      if (depth == 1 && strncmp(name, "memory@", 7) == 0) in_memory = 1;
      depth++;
      pos = (pos + nlen + 1 + 3) & ~(size_t)3;
    } else if (tok == kFdtEndNode) {
      depth--;
      if (depth <= 1) in_memory = 0;
    } else if (tok == kFdtProp) {
      uint32_t len = Be32(dtb + pos);
      uint32_t nameoff = Be32(dtb + pos + 4);
      pos += 8;
      const char *nm = (const char *)(dtb + strings + nameoff);
      // reg = <base_hi base_lo size_hi size_lo> under #address-cells=2,
      // #size-cells=2: the size is the second 8-byte cell.
      if (in_memory && strcmp(nm, "reg") == 0 && len >= 32)
        PutBe64(dtb + pos + 16, ram_size);
      pos = (pos + len + 3) & ~(size_t)3;
    } else if (tok != kFdtNop) {
      break;  // kFdtEnd or corruption: stop
    }
  }
}

// ---- creation ----

Board *VirtBoardCreate(const BoardOpts *opts) {
  if (opts->ram_base && opts->ram_base != kDramBase) {
    LogError("virt machine DRAM is fixed at 0x80000000");
    return NULL;
  }
  uint64_t ram_size = opts->ram_size ? opts->ram_size : kDramSizeDefault;
  VirtBoard *vm = (VirtBoard *)calloc(1, sizeof(VirtBoard));
  if (!vm) return NULL;
  Board *m = &vm->base;
  m->name = "virt";

  m->ram = RamCreate(kDramBase, ram_size);
  vm->mrom = RamCreate(kMromBase, kMromSize);
  if (!m->ram || !vm->mrom) {
    RamDestroy(m->ram);
    RamDestroy(vm->mrom);
    free(vm);
    return NULL;
  }
  BusAddRamRegion(&m->bus, kDramBase, ram_size, &kRamOps, m->ram, m->ram->mem);
  BusAddRamRegion(&m->bus, kMromBase, kMromSize, &kRamOps, vm->mrom,
                  vm->mrom->mem);
  m->bin_base = kDramBase;  // firmware loads at the DRAM base

  // Mask-ROM contents: the reset vector, then the data slots it references.
  // start_addr (0x1018) is the firmware load address (m->bin_base, so
  // --bin-base overrides reach the trampoline), fdt_addr (0x1020) is where
  // the DTB will land below, and a2 points at fw_dynamic_info (0x1028).
  uint8_t *mrom = vm->mrom->mem;
  memcpy(mrom, kResetVec, sizeof(kResetVec));
  uint64_t fdt_addr = (kDramBase + ram_size - kFdtReserve) & ~(kFdtReserve - 1);
  uint64_t slots[3 + 6];
  slots[0] = m->bin_base;
  slots[1] = fdt_addr;
  memcpy(slots + 2, kFwDynInfo, sizeof(kFwDynInfo));
  memcpy(mrom + 0x18, slots, sizeof(slots));

  // Place the DTB where the trampoline promises it, with the memory node
  // describing the actual RAM size.
  if (kFdtReserve < kVirtDtbSize || ram_size < kFdtReserve + kVirtDtbSize) {
    LogError("virt machine DRAM too small for the device tree");
    VirtDestroy(m);
    RamDestroy(m->ram);
    free(vm);
    return NULL;
  }
  uint8_t *fdt = m->ram->mem + (fdt_addr - kDramBase);
  memcpy(fdt, kVirtDtb, kVirtDtbSize);
  PatchDtbMemorySize(fdt, ram_size);

  // Devices (QEMU virt DTB): CLINT, PLIC, UART0, sifive test finisher.
  ClintInit(&vm->clint, kTimebaseHz);
  ClintRegister(&m->bus, &vm->clint, kClintBase, kClintSize);
  ClintSetIrqSink(&vm->clint, OnClintIrq, &m->cpu);
  PlicInit(&vm->plic);
  PlicRegister(&m->bus, &vm->plic, kPlicBase, kPlicSize);
  PlicSetIrqSink(&vm->plic, OnPlicIrq, &m->cpu);
  Uart16550Init(&vm->uart);
  BusAddRegion(&m->bus, kUartBase, kUartSize, &kUart16550Ops, &vm->uart);
  Uart16550SetIrqSink(&vm->uart, OnUartIrq, &vm->plic);
  SifiveTestBind(&vm->test, &m->cpu);
  SifiveTestRegister(&m->bus, &vm->test, kTestBase, kTestSize);

  m->cpu.halted = kCpuRunning;
  m->cpu.bus = &m->bus;
  m->cpu.timer_read = ReadTime;
  m->cpu.timer_dev = &vm->clint;
  m->reset_pc = kMromBase;  // reset enters the mask-ROM trampoline
  m->bin_htif = 0;  // virt binaries follow the QEMU boot contract, not HTIF
  m->poll = VirtPoll;
  m->destroy = VirtDestroy;
  return m;
}
