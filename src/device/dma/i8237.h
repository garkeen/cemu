#ifndef CEMU_DEVICE_DMA_I8237_H
#define CEMU_DEVICE_DMA_I8237_H

#include <stdint.h>

#include "bus/bus.h"

// Intel 8237A DMA controller: the PC's ISA bus-master engine, two chips
// cascaded (four channels each). A peripheral raises DRQ, the controller
// answers DACK and moves one byte per cycle between the device and system
// memory; the address and the count come from the channel's own registers,
// which the guest programs before it starts the device. That is the whole
// contract a floppy transfer rests on: the FDC never touches memory itself,
// it only says "move this many bytes" and the 8237 walks the bus.
//
// The 8237A addresses 64KiB with its 16-bit address register, so the high
// byte of a 24-bit ISA address comes from a separate page latch outside the
// chip (Intel 8237A datasheet "Page Registers"; PC/AT Technical Reference).
// Those latches live at fixed ports and are modelled here because the two
// halves are one address as far as the guest is concerned.
//
// Register map (Intel 8237A datasheet; offsets from each chip's base port):
//
//   +0x0/+0x1 .. +0x6/+0x7  channel 0..3 address / count (two byte accesses
//                           per register, low byte first, one flip-flop shared
//                           by all of them)
//   +0x8   write: command    read: status (bits 0..3 = channel reached
//                           terminal count)
//   +0x9   write: request (software DRQ)
//   +0xA   write: single-channel mask (bit 2 = set mask, bits 1:0 = channel)
//   +0xB   write: mode (bits 1:0 channel, 3:2 direction, 4 autoinit,
//                 5 address decrement, 7:6 mode)
//   +0xC   write: clear the byte flip-flop
//   +0xD   write: master clear (resets every register, clears the masks)
//   +0xE   write: clear all four mask bits
//   +0xF   read/write: the four mask bits as one byte
//
// The two chips are one device here — the second is the first's cascade
// slave, its channels 4..7 reach the bus through the master's channel 0, and
// the page latches for all eight channels sit in the same port block.
enum {
  kI8237Channels = 8,
  // Channels 0..3 belong to the master chip (ports 0x00-0x0F), 4..7 to the
  // slave (0xC0-0xDF); the page latches are addressed by channel number.
  kI8237MasterChannels = 4,
  // A channel's count register holds "bytes to move minus one", so a
  // transfer of N bytes is programmed as N-1 (Intel 8237A datasheet).
  kI8237CountBias = 1,
};

// Mode register fields (Intel 8237A datasheet).
enum {
  kI8237ModeChannel = 0x03,
  kI8237ModeDirection = 0x0c,
  kI8237ModeAutoInit = 0x10,
  kI8237ModeDecrement = 0x20,
  kI8237ModeKind = 0xc0,
};
// Direction field: the transfer type as seen from the controller. The pair
// 11 is the datasheet's illegal combination and is not named here.
enum {
  kI8237DirVerify = 0x00,  // verify: the controller moves no data at all
  kI8237DirWrite = 0x04,   // device -> memory
  kI8237DirRead = 0x08,    // memory -> device
};
// The single-channel mask register's set/clear bit.
enum { kI8237MaskSet = 0x04 };

// Command register bit 2: zero enables the controller, one disables it
// (Intel 8237A datasheet) — which is why a master clear leaves a working
// chip behind and why the reset value here is usable without programming.
enum { kI8237CmdDisable = 0x04 };

// One channel: the two 16-bit registers (with the values autoinit reloads
// from), the page latch that extends the address, the mode byte and the mask
// bit. A transfer walks addr/count and leaves them where it stopped, which is
// what a guest that reads them back after a command sees.
typedef struct I8237Channel {
  uint16_t addr;
  uint16_t addr_init;
  uint16_t count;
  uint16_t count_init;
  uint8_t page;
  uint8_t mode;
  int masked;
} I8237Channel;

typedef struct I8237Device {
  I8237Channel ch[kI8237Channels];
  uint8_t command;
  uint8_t status;   // the terminal-count bits, cleared by a master clear
  uint8_t request;  // software DRQ bits
  // One flip-flop for every two-byte register on both chips: the guest writes
  // low then high, and the clear-flip-flop port resets it so the next access
  // starts low again (Intel 8237A datasheet; SeaBIOS src/hw/dma.c depends on
  // it, clearing it before each address and count pair).
  int flipflop;
  // The address space a transfer walks. The controller is the one device that
  // reads and writes guest memory on a peripheral's behalf, so the board
  // hands it the system bus (like the IDE bus-master engine).
  Bus* mem;
} I8237Device;

void I8237Init(I8237Device* d);
// A machine reset: both chips back to power-on (a master clear on each),
// keeping the memory bus the board wired.
void I8237Reset(I8237Device* d);
// The three port windows: the master's registers, the slave's, and the page
// latches for all eight channels.
void I8237Register(Bus* io, I8237Device* d);
// The memory bus a transfer moves data through.
void I8237SetMemBus(I8237Device* d, Bus* mem);
// Moves up to `len` bytes on `channel` between guest memory and `buf`, the
// way the controller answers a device's DRQ/DACK handshake. The direction is
// the channel's own mode bit: a peripheral only says how much it wants to
// move, and the programming decides which way the bytes go (Intel 8237A
// datasheet), so the caller must have set the mode register first — for a
// floppy that is SeaBIOS src/hw/dma.c dma_floppy's 0x46 for a read and 0x4a
// for a write. The address and count decide where the data goes and how much
// of it moves, and both are left updated for the next round. Returns the
// bytes moved, 0 when the channel is masked, the controller is disabled, the
// mode asks for a verify, or the count is already spent.
int I8237Transfer(I8237Device* d, int channel, uint8_t* buf, int len);

#endif
