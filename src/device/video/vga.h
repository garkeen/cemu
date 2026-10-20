#ifndef CEMU_DEVICE_VIDEO_VGA_H
#define CEMU_DEVICE_VIDEO_VGA_H

#include <stdint.h>

#include "bus/bus.h"
#include "device/misc/pci.h"
#include "device/video/display.h"

// IBM VGA family core (阶段 4 图形切片). CGA, EGA and VGA are three faces of
// one card family, so they are three compatibility layers of this core rather
// than three separate cards (user ruling, progress.md 片 25): one plane-based
// VRAM, the four register groups at 0x3C0-0x3DF, planar and chained
// addressing, and a 256-entry colour DAC. EGA 640x350 — the mode Windows
// 1.01's display driver asks for (acceptance 4) — is the same register model
// as VGA 640x480, and the CGA modes 4/5/6 are its 2bpp/1bpp planar shapes.
//
// Register map and addressing: IBM VGA Technical Reference (the four groups,
// the memory maps, the odd/even text layout), IBM CGA Technical Reference (the
// 0x3D8/0x3D9 compatibility latches and the CGA colour model), FreeVGA's
// register reference. The behavioural cross-check for the graphics
// controller's write/read modes and the register masking is QEMU
// hw/display/vga.c (the model dearchap-tinyemu and tiny386 carry as well).
//
// Ports:
//   0x3B4/0x3B5  CRTC index/data (monochrome decode)
//   0x3C0/0x3C1  attribute controller index/data (shared flip-flop)
//   0x3C2        miscellaneous output (write) / input status 0 (read)
//   0x3C4/0x3C5  sequencer
//   0x3C6-0x3C9  DAC: PEL mask, read index, write index, data
//   0x3CA        feature control (read)
//   0x3CC        miscellaneous output (read)
//   0x3CE/0x3CF  graphics controller
//   0x3D0-0x3D3  CGA CRTC mirrors; 0x3D4/0x3D5 CRTC (colour decode)
//   0x3D8/0x3D9  CGA mode/colour latches (CGA compatibility)
//   0x3DA        input status 1 (read) / feature control (write)
//
// Memory windows: A0000-AFFFF (graphics) and B0000-B7FFF / B8000-BFFFF (text)
// resolve into the planes through the graphics controller's memory mode; in
// text modes the odd/even scheme keeps each cell's character in plane 0 and
// its attribute in plane 1 — which is also what CEMU_DEBUG's `screen` mirror
// reads back (AGENTS.md §X).
enum {
  // Planes are stored dword-interleaved: vram[offset * 4 + plane] is plane
  // `plane` at offset, and chain-4 addressing (which selects the plane with
  // address bits 1:0) therefore reads and writes vram[offset] directly. The
  // widest window the graphics controller's mapping modes describe,
  // 0xA0000-BFFFF, names 128KB of offsets in each of the four planes, so the
  // planar view alone occupies the first 512KB of the array.
  //
  // The card's whole video memory is what its framebuffer BAR is sized to and
  // what the DISPI VIDEO_MEMORY_64K register reports, so it is one number and
  // not two. 16MB is what the reference card answers, measured twice on
  // qemu-system-i386 -vga std: SeaBIOS sizes 00:02.0's BAR0 to
  // "size 01000000 [prefmem]" and DISPI index 0x0a reads 256
  // (test/x86/probe/vga_dispi_qemu.txt). The number is not cosmetic -- the
  // option ROM builds its mode table against the memory the card reports, so a
  // 256KB card makes SeaVGABIOS print "Removing mode 100..113" and the VBE mode
  // list the guest sees stops matching the reference machine's.
  kVgaVramSize = 16 * 1024 * 1024,
  // Widest picture this renderer draws. Wider modes are clipped rather than
  // overrunning the buffer (AGENTS.md 简化登记 D17).
  kVgaFbWidth = 800,
  kVgaFbHeight = 600,
  // The text mirror's grid: enough for 132x60 (the largest text mode the CRTC
  // registers can describe here); the mirror prints the current mode's grid.
  kVgaMirrorCols = 132,
  kVgaMirrorRows = 60,
};

// DISPI, the register pair the option ROM probes to decide the card can offer
// VBE. Register indices (bochsvga.h VBE_DISPI_INDEX_*); ENABLE bits
// (VBE_DISPI_*): 0x01 enabled, 0x02 get-caps (write-only), 0x20 8-bit DAC,
// 0x40 linear framebuffer, 0x80 do-not-clear.
enum {
  kDispiIndexPort = 0x1ce,
  kDispiDataPort = 0x1cf,
  // Four bytes, not two: the bus requires a whole access to fit one region, and
  // the option ROM's inw at 0x1CF covers 0x1CF-0x1D0.
  kDispiPortCount = 4,
  kDispiRegCount = 0x0b,
};
enum {
  kDispiId = 0,
  kDispiXres = 1,
  kDispiYres = 2,
  kDispiBpp = 3,
  kDispiEnable = 4,
  kDispiBank = 5,
  kDispiVirtWidth = 6,
  kDispiVirtHeight = 7,
  kDispiXOffset = 8,
  kDispiYOffset = 9,
  kDispiVideoMemory64K = 10,
};
enum {
  kDispiEnabled = 0x01,
  kDispiGetCaps = 0x02,
  kDispiLfbEnabled = 0x40,
  kDispiNoClearMem = 0x80,
};

// The card's PCI function. A VGA that only exists as an ISA device is not a
// complete machine: firmware and guests alike find a display adapter by
// enumerating the bus for a display-controller class device, and QEMU's PC has
// one (hw/display/vga-pci.c, the i440FX-era 1234:1111 the SeaVGABIOS DISPI code
// also recognises -- seabios/vgasrc/bochsvga.c's vendor switch has a `case
// 0x1234: /* stdvga */` that reads BAR 2 for the register block and BAR 0 for
// the framebuffer). The identity below is that one, so the same option ROM
// serves both.
enum {
  kPciVendorStdVga = 0x1234,
  kPciDeviceStdVga = 0x1111,
  kVgaPciRevision = 0x02,  // what QEMU's stdvga reports
};
// Configuration-space byte offsets (PCI spec §6.2, type 0 header).
enum {
  kCfgVendor = 0x00,
  kCfgDevice = 0x02,
  kCfgCommand = 0x04,
  kCfgRevision = 0x08,
  kCfgProgIf = 0x09,
  kCfgSubClass = 0x0a,
  kCfgClass = 0x0b,
  kCfgHeaderType = 0x0e,
  kCfgBar0 = 0x10,
  // BAR1 is an absent slot: BAR0 is a 32-bit prefetchable memory BAR (QEMU's
  // pci_register_bar flags are PCI_BASE_ADDRESS_MEM_PREFETCH only), so this is
  // not the upper half of a 64-bit pair. It reads/writes zero like BAR3-BAR5.
  kCfgBar1 = 0x14,
  kCfgBar2 = 0x18,
  kCfgBar3 = 0x1c,
  kCfgBar4 = 0x20,
  kCfgBar5 = 0x24,
  // 0x28 is the CardBus CIS pointer in a type-0 header (PCI spec §6.2.1), not
  // a sixth BAR; it stays read-only zero like the absent BAR slots.
  kCfgCardBusCis = 0x28,
  kCfgSubsystemVendor = 0x2c,
  kCfgSubsystemId = 0x2e,
  kCfgExpRomBase = 0x30,
  kCfgInterruptLine = 0x3c,
  kCfgInterruptPin = 0x3d,
};
enum {
  // QEMU's standard-VGA specification: PCI Region 2 is a 4096-byte *MMIO* bar
  // (not an I/O bar). The first 1KB is the EDID blob; 0x400-0x41f are VGA
  // ports 0x3c0-0x3df remapped 1:1. EDID is not modelled (reads answer open
  // bus, registered in AGENTS.md §X D17), the port window is.
  kVgaBar2Size = 0x1000,
  kVgaBar2EdidSize = 0x400,
  kVgaBar2PortBase = 0x400,
  kVgaBar2PortCount = 0x20,
  kVgaBar2FirstPort = 0x3c0,
};
// A BAR's read-only type field (bits 2-1 for memory, bit 0 for I/O; PCI spec
// §6.2.5.2's type encoding and Table 7-7). QEMU registers this card's BAR0 with
// PCI_BASE_ADDRESS_MEM_PREFETCH only: 32-bit prefetchable, not 64-bit.
enum {
  kBarTypePrefetch = 0x8,  // 32-bit memory, prefetchable
};

typedef struct VgaDevice {
  // The card's video memory as four planes, laid out dword-interleaved the
  // way the graphics controller reaches them: vram[offset * 4 + plane] is
  // plane `plane` at offset. The read latch holds the four plane bytes the
  // last fetch read; read mode 1 and write modes 1-3 act on it.
  uint8_t vram[kVgaVramSize];
  uint32_t latch;
  uint8_t cr[0x19];  // CRTC R0-R24 (the chip has no more registers)
  uint8_t cr_index;
  uint8_t sr[8];  // sequencer SR0-SR7
  uint8_t sr_index;
  uint8_t gr[0x10];  // graphics controller GR0-GR15
  uint8_t gr_index;
  uint8_t ar[0x15];  // attribute controller AR0-AR20
  uint8_t ar_index;
  int ar_flip_flop;
  uint8_t misc;     // miscellaneous output (0x3C2 write / 0x3CC read)
  uint8_t feature;  // feature control (0x3CA read / 0x3DA write)
  uint8_t st00;     // input status 0: latched when 0x3C2 is read
  uint8_t st01;     // input status 1: vertical retrace, display enable
  uint8_t cga_mode;   // 0x3D8 latch: CGA video enable / 40-col / graphics / blink
  uint8_t cga_color;  // 0x3D9 latch: CGA border, palette select, intensity
  // DAC (0x3C6-0x3C9): 256 6-bit RGB entries, the PEL mask, and the read and
  // write index state machines (each data byte is one colour component).
  uint8_t dac[256 * 3];
  uint8_t dac_mask;
  uint8_t dac_read_index;
  uint8_t dac_write_index;
  uint8_t dac_sub_index;
  uint8_t dac_state;
  uint8_t dac_cache[3];
  // Rendering: the guest's register writes mark the picture dirty; Poll
  // re-renders at most once per interval into fb, whose size is the current
  // mode's (fb_width x fb_height) and which the display channel publishes.
  int dirty;
  uint32_t version;
  uint32_t fb[kVgaFbWidth * kVgaFbHeight];
  int fb_width;
  int fb_height;
  // The last picture the mode report (CEMU_DEBUG=mark, "vga-mode") described,
  // so a guest that keeps its mode does not repeat the row.
  int marked_flags;
  int marked_width;
  int marked_height;
  uint64_t last_render_us;
  uint32_t rendered_blink_tick;
  // The text grid the `screen` mirror last printed, so only changed rows are
  // emitted.
  char mirror[kVgaMirrorRows][kVgaMirrorCols + 1];
  // DISPI: the VESA BIOS Extension's register block, an index/data I/O pair at
  // 0x1CE/0x1CF (seabios/vgasrc/bochsvga.h; QEMU hw/display/vga.c's 0x1ce/0x1cf
  // case is the behavioural reference). An option ROM probes it by writing one
  // of the six ids and reading it back -- seabios/vgasrc/bochsvga.c:309-313 --
  // and that handshake is the whole gate on VBE: no DISPI and the firmware
  // prints "No VBE DISPI interface detected", registers no int 10h AH=4Fh, and
  // the guest is told the card has no linear framebuffer.
  //
  // The registers are 16 bits wide because the ROM reaches them with outw/inw
  // and the ids alone (0xB0C0) do not fit in a byte. A byte access does not
  // reach them: the byte path has no case for these two ports, so it reads high
  // and writes nothing (registered in AGENTS.md §X's D17). Nothing indexes
  // `dispi` by a raw position either -- vga.c's DispiReg is the only door, and
  // the index is bounded on the way in, because an out-of-range slot here lands
  // in the PCI config space just below.
  uint8_t dispi_index;   // the register the data port addresses
  uint16_t dispi_id;     // the id last written; the read answers with it
  uint16_t dispi[kDispiRegCount];
  // The PCI function, and the memory bus its framebuffer BAR is mapped on when
  // firmware (or a guest) programs the address. lfb_base is the address last
  // programmed, so a second write to the same BAR does not map it twice.
  PciDevice pci;
  Bus* mem_bus;
  uint64_t lfb_base;
  // The programmed Region-2 MMIO window (QEMU standard VGA: 4KB at 0x000 EDID,
  // 0x400-0x41f VGA ports). 0 = not programmed.
  uint32_t bar2_base;
} VgaDevice;

void VgaInit(VgaDevice* d);
// Fills in the PCI function's identity, class and BARs, and remembers the memory
// bus so a programmed framebuffer BAR can be mapped there. The board calls this
// and PciAddDevice; without it the card is an ISA device only (VgaPciInit,
// vga.h's PCI section for why that is not the whole machine).
void VgaPciInit(VgaDevice* d, Bus* mem_bus, uint8_t bus, uint8_t dev);
// Takes the card's programmed BAR windows back off the bus without touching the
// PCI configuration. A machine reset calls this before VgaInit wipes the device
// (the bus pointer lives in the device), then VgaPciInit rebuilds the function
// and the board re-enumerates it: a reset drops the decode but not the device.
void VgaPciDetach(VgaDevice* d);
// Maps the three memory windows on the memory bus (on top of RAM: the bus
// resolves the smaller window first) and the card's port block at
// 0x3B0-0x3DF (0x3B0-0x3BF and 0x3D0-0x3DF are one decode on a VGA: the
// colour/monochrome bit in the miscellaneous output register decides which
// half answers) on the I/O bus.
void VgaRegister(Bus* bus, Bus* io, VgaDevice* d);
// Advances the raster status from the host clock and re-renders when the guest
// touched the register file or the blink phase flipped. Board poll hook, like
// PitPoll.
void VgaPoll(VgaDevice* d);

extern const DeviceOps kVgaPortOps;
// The aperture's device ops (0xA0000-0xBFFFF): VgaRegister maps it, and the
// board never touches it directly.
extern const DeviceOps kVgaMemOps;
extern const DisplaySourceOps kVgaDisplayOps;

#endif
