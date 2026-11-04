#ifndef CEMU_BOOT_H
#define CEMU_BOOT_H

// cemu_boot_info v1: the firmware -> kernel handoff record (arch.md 阶段 5).
//
// The record is pure data: the firmware fills it, the kernel reads it once
// early (copying it into its own memory) and then owns the machine.  No
// firmware code is called after the jump; the only exception is the riscv
// M-mode SBI subset, which is an ISA-level ecall interface, not a pointer
// into firmware logic.
//
// Rules both sides implement:
//   * magic/version/size are validated by the kernel; a mismatch is fatal.
//   * arrays are addressed by physical address + count + stride, so a newer
//     firmware can append fields without breaking an older kernel.
//   * a kind the consumer does not know is skipped, never guessed.
//   * all physical addresses are 64-bit; a 32-bit kernel truncates (the x86
//     board keeps everything below 4 GiB).
//   * the record and everything it points at is immutable after the jump.
//
// Handoff register state:
//   x86 (i386): 32-bit protected mode, paging off, flat CS=0x08 / DS..GS=0x10,
//               IF=0, ESP = kernel stack, EAX = CEMU_BOOT_MAGIC,
//               EBX = physical address of this record.
//   riscv64:    S-mode, satp = 0, sp = kernel stack, a0 = hartid,
//               a1 = physical address of this record.

#include <stdint.h>

#define CEMU_BOOT_MAGIC 0x43454D55u   // "CEMU"
#define CEMU_BOOT_VERSION 0x00010000u  // major 1, minor 0

// ---------------------------------------------------------------------------
// Memory map
// ---------------------------------------------------------------------------

enum {
  CEMU_MEM_USABLE = 1,    // RAM the kernel may allocate
  CEMU_MEM_RESERVED = 2,  // firmware, SBI, MMIO holes: never allocate
  CEMU_MEM_MMIO = 3,      // device window mapped into physical space
  CEMU_MEM_ACPI = 4,      // ACPI reclaimable (x86; 0 entries today)
};

typedef struct CemuMemEntry {
  uint64_t base;
  uint64_t len;
  uint32_t type;  // CEMU_MEM_*
  uint32_t reserved;
} CemuMemEntry;

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

enum {
  CEMU_CONSOLE_NONE = 0,
  CEMU_CONSOLE_UART_MMIO = 1,  // 16550-compatible registers in MMIO
  CEMU_CONSOLE_UART_PORT = 2,  // 16550-compatible registers in I/O space
};

typedef struct CemuConsole {
  uint32_t kind;      // CEMU_CONSOLE_*
  uint32_t irq_kind;  // CEMU_IRQ_*
  uint32_t irq;       // line number in irq_kind's space, 0 = none
  uint32_t shift;     // register stride log2 (0 = byte per register)
  uint64_t base;      // MMIO address or I/O port
  uint64_t size;
} CemuConsole;

// ---------------------------------------------------------------------------
// Interrupt topology
// ---------------------------------------------------------------------------

enum {
  CEMU_IRQ_NONE = 0,
  CEMU_IRQ_ISA = 1,   // 8259 line / ISA IRQ number
  CEMU_IRQ_GSI = 2,   // I/O APIC global system interrupt
  CEMU_IRQ_PLIC = 3,  // PLIC source number
};

enum {
  CEMU_INTC_NONE = 0,
  CEMU_INTC_PIC = 1,     // 8259 pair; base = I/O ports (cmd at base+0/0x80)
  CEMU_INTC_IOAPIC = 2,  // base = MMIO window
  CEMU_INTC_PLIC = 3,    // base = MMIO window; ctx = S-mode context index
};

typedef struct CemuIntc {
  uint32_t kind;  // CEMU_INTC_*
  uint32_t out;   // line/vector this controller raises on the CPU
  uint32_t ctx;   // context index (PLIC); 0 for x86 controllers
  uint32_t reserved;
  uint64_t base;
  uint64_t size;
} CemuIntc;

// ---------------------------------------------------------------------------
// Block devices
// ---------------------------------------------------------------------------

enum {
  CEMU_BLK_NONE = 0,
  CEMU_BLK_VIRTIO_MMIO = 1,  // virtio-mmio slot, modern or legacy transport
  CEMU_BLK_ATA_PIO = 2,      // 28-bit LBA PIO, task file at base
};

typedef struct CemuBlk {
  uint32_t kind;          // CEMU_BLK_*
  uint32_t irq_kind;      // CEMU_IRQ_*
  uint32_t irq;           // PLIC source / GSI / ISA line
  uint32_t sector_shift;  // log2(bytes per sector), normally 9
  uint64_t base;          // virtio-mmio slot base, or ATA command block port
  uint64_t sectors;
  uint64_t size;          // device window size (virtio); 0 for ATA
} CemuBlk;

// ---------------------------------------------------------------------------
// Framebuffer
// ---------------------------------------------------------------------------

enum {
  CEMU_FB_NONE = 0,
  CEMU_FB_XRGB8888 = 1,  // 32 bits per pixel, 0x00RRGGBB
};

enum {
  CEMU_FB_FLAG_READY = 1u << 0,  // the firmware already programmed the mode
};

typedef struct CemuFb {
  uint64_t base;
  uint32_t width;
  uint32_t height;
  uint32_t stride;  // bytes per scan line
  uint32_t format;  // CEMU_FB_*
  uint32_t flags;   // CEMU_FB_FLAG_*
  uint32_t reserved;
} CemuFb;

// ---------------------------------------------------------------------------
// PCI enumeration
// ---------------------------------------------------------------------------

enum {
  CEMU_PCI_NONE = 0,
  CEMU_PCI_CF8CFC = 1,  // 0xCF8/0xCFC mechanism; base = 0
  CEMU_PCI_ECAM = 2,    // memory-mapped ECAM; base = window
};

typedef struct CemuPci {
  uint32_t kind;  // CEMU_PCI_*
  uint32_t seg;   // ECAM segment
  uint32_t bus_first;
  uint32_t bus_last;
  uint64_t base;
  uint64_t size;
} CemuPci;

// ---------------------------------------------------------------------------
// The record
// ---------------------------------------------------------------------------

typedef struct CemuBootInfo {
  uint32_t magic;    // CEMU_BOOT_MAGIC
  uint32_t version;  // CEMU_BOOT_VERSION
  uint32_t size;     // sizeof(CemuBootInfo) as built by the firmware
  uint32_t flags;    // reserved, 0

  // Memory map: array of CemuMemEntry at physical `mem`, `mem_count` entries,
  // `mem_stride` bytes apart (stride >= sizeof(CemuMemEntry)).
  uint64_t mem;
  uint32_t mem_count;
  uint32_t mem_stride;

  CemuConsole console;

  // Block devices: array of CemuBlk.
  uint64_t blk;
  uint32_t blk_count;
  uint32_t blk_stride;

  CemuFb fb;

  // Interrupt controllers: array of CemuIntc.
  uint64_t intc;
  uint32_t intc_count;
  uint32_t intc_stride;

  CemuPci pci;

  // Firmware-provided tables.  riscv: physical DTB the board passed in a1.
  // x86: physical ACPI RSDP once the firmware builds one (0 today).
  uint64_t dtb;
  uint64_t acpi_rsdp;

  // NUL-terminated command line, physical address.
  uint64_t cmdline;

  // Optional initial ramdisk (loaded by the firmware next to the kernel).
  uint64_t initrd;
  uint64_t initrd_len;

  // Wall clock at handoff, seconds since the Unix epoch (0 when the machine
  // has no RTC the firmware can read, e.g. virt without a goldfish RTC).
  uint64_t rtc_unix;

  // Root filesystem: index into the block array, plus the partition window
  // the firmware mounted it from (0/0 when the whole disk is the filesystem).
  uint32_t root_blk;
  uint32_t reserved;
  uint64_t root_part_lba;
  uint64_t root_part_sectors;

  // riscv: the hart this record was built for; x86: 0.
  uint32_t hartid;
  uint32_t reserved2;
} CemuBootInfo;

#endif
