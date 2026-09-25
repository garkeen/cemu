#ifndef CEMU_DEVICE_STORAGE_FDC_H
#define CEMU_DEVICE_STORAGE_FDC_H

#include <stdint.h>

#include "bus/bus.h"
#include "host/host.h"

// Intel 8272A / NEC µPD765 floppy disk controller, the PC's standard floppy
// interface. The guest drives it through a small port file and, for anything
// that moves sector data, through the 8237: a read or write command names a
// cylinder/head/sector, the controller answers DRQ and the DMA controller
// walks system memory on its behalf, so the FDC itself never touches guest
// RAM. That division is the whole reason D21 covers both devices.
//
// Ports (PC/AT Technical Reference; QEMU hw/block/fdc.c maps the same set):
//
//   0x3F0  read:  Status Register A (interrupt pending)
//   0x3F1  read:  Status Register B (motor and drive-select lines)
//   0x3F2  read/write: Digital Output Register (drive select, motor, DMA and
//                 IRQ enable, and the reset line — a 0-to-1 transition there
//                 restarts the controller)
//   0x3F3  read/write: Tape Drive Register (media boot selection)
//   0x3F4  read:  Main Status Register (RQM/DIO and the busy bits — the
//                 handshake every command and result byte goes through)
//   0x3F5  read/write: the data FIFO (command bytes and parameters in,
//                 result bytes out)
//   0x3F7  read:  Digital Input Register (bit 7: no disk, or the disk was
//                 changed since the last step)
//   0x3F6 is the IDE controller's control block and is not part of this file.
//
// The byte-level contract with the firmware is what matters here, and it is
// entirely visible in SeaBIOS src/hw/floppy.c: it polls RQM before every
// access, refuses to write while DIO is set and refuses to read while it is
// clear, waits for an interrupt after the commands that raise one, and after
// a controller reset issues four SENSE INTERRUPT STATUS commands to retire
// the four logical drives' pending reset interrupts (floppy_enable_controller,
// "section 7.4 Drive Polling" of the Intel 82077AA datasheet).
//
// The media model is a whole image held open read-write, like the IDE
// controller's: a floppy is a whole disk in a drive, and the guest's writes
// belong in the file.
enum {
  // A PC has two floppies at most (A: and B:); the controller's drive-select
  // field is two bits but only these two are wired.
  kFdcDrives = 2,
  // 512-byte sectors are what every format SeaBIOS knows uses (its
  // FLOPPY_SIZE_CODE 2), and the only size this model is asked for.
  kFdcSectorSize = 512,
  // One transfer can carry a whole track on both heads when the command's
  // multi-track bit is set: the largest format accepted here is 2.88MB, whose
  // 36 sectors double to 72 (seabios src/hw/floppy.c FloppyInfo[5]).
  kFdcMaxTransfer = 2 * 36 * kFdcSectorSize,
};

// The controller's two lines into the machine.
enum { kFdcIrqLine = 6, kFdcDmaChannel = 2 };

// One drive: its medium, the geometry that medium implies, and where the head
// currently is. The head position is what SENSE INTERRUPT STATUS reports and
// what a read or write without an explicit seek continues from, so it is part
// of the device's state, not of any one command.
typedef struct FdcDrive {
  HostFile* image;  // the medium; NULL = empty drive
  int64_t bytes;
  // Geometry of the inserted medium (see the format table in fdc.c): the
  // controller is told nothing about it, the guest learns it from READ ID.
  int cylinders;
  int heads;
  int sectors;
  int track;
  int head;
  int sector;
  // The disk-change line: latched when a step pulse finds no medium, cleared
  // by a step with one inserted. SeaBIOS reads it through the DIR to tell an
  // empty drive from a full one without waiting for a command to time out
  // (src/hw/floppy.c floppy_check_media).
  int media_changed;
  int read_only;
  // The CMOS drive type this medium corresponds to, for the equipment byte
  // firmware reads before it ever touches the controller (0 = no drive).
  uint8_t cmos_type;
} FdcDrive;

typedef struct FdcDevice {
  FdcDrive drives[kFdcDrives];
  uint8_t sra;
  uint8_t srb;
  uint8_t dor;
  uint8_t tdr;
  uint8_t msr;
  uint8_t status0;
  uint8_t status1;
  uint8_t status2;
  // Command phase: the code being assembled, its parameters and how many are
  // still expected. The controller takes one byte per access and only runs
  // the command when the last parameter arrives.
  uint8_t cmd_code;
  int cmd_flags;
  uint8_t cmd_args[16];
  int cmd_cursor;
  int cmd_remaining;
  int phase;
  // Result phase: the bytes a command leaves for the guest to read back, in
  // order, and how far the guest has got through them.
  uint8_t resp[16];
  int resp_cursor;
  int resp_len;
  int curr_drive;
  // How many of the four reset interrupts are still owed to the guest; a
  // controller reset arms all four (SeaBIOS retires them with four SENSE
  // INTERRUPT STATUS commands).
  int reset_sense;
  // The end-of-track sector of the command in flight, which decides where a
  // multi-sector transfer stops.
  uint8_t eot;
  int irq;  // the IRQ6 line's current level
  void (*set_irq)(void* ctx, int line, int level);
  void* irq_ctx;
  // The DMA channel's byte mover. It is the 8237 that decides the direction
  // from its own mode register and walks guest memory; the controller only
  // hands it a buffer and says how much it wanted to move. Returns the bytes
  // actually moved.
  int (*dma)(void* ctx, uint8_t* buf, int len);
  void* dma_ctx;
  // A whole track's worth of sector data, which is the largest single
  // transfer a command can ask for.
  uint8_t buf[kFdcMaxTransfer];
} FdcDevice;

// Power-on state; safe to call again for a machine reset (D18).
void FdcInit(FdcDevice* d);
// A machine reset: the controller back to power-on, no interrupt raised (a
// machine reset is not a guest-commanded controller reset).
void FdcReset(FdcDevice* d);
// Attaches a medium to drive 0 (A:) or 1 (B:). The image must match one of
// the known floppy formats; returns 0 on success.
int FdcAttach(FdcDevice* d, int drive, const char* path);
// The port windows: 0x3F0-0x3F5 and 0x3F7 (0x3F6 belongs to the IDE
// controller).
void FdcRegister(Bus* io, FdcDevice* d);
void FdcSetIrqSink(FdcDevice* d, void (*set_irq)(void* ctx, int line, int level), void* ctx);
// The byte mover of the DMA channel the board wired to this controller.
void FdcSetDmaSink(FdcDevice* d, int (*dma)(void* ctx, uint8_t* buf, int len), void* ctx);
// The CMOS drive type of an attached drive (0 when the drive is empty).
uint8_t FdcCmosType(const FdcDevice* d, int drive);
// Closes the attached images.
void FdcDestroy(FdcDevice* d);

#endif
