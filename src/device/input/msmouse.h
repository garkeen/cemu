#ifndef CEMU_DEVICE_INPUT_MSMOUSE_H
#define CEMU_DEVICE_INPUT_MSMOUSE_H

#include <stdint.h>

// Microsoft serial mouse: the 1985-era pointing device that hangs off a serial
// port, which is what a guest of that vintage drives (Windows 1.01 predates the
// PS/2 port by two years, so its mouse driver speaks this, not ps2mouse.c's).
// It is not a bus device: it sends 3-byte packets *out* of COM1's receiver, as
// if the bytes had arrived on the SIN pin, so it has a byte sink and no
// registers.
//
// The packet (the Microsoft "M" protocol, 1200 baud, 7 data bits, no parity,
// one stop bit):
//
//   byte 1: 0 1 Left Right dy7 dy6 dx7 dx6   (bit 6 is always set)
//   byte 2: 0 0 dx5 dx4 dx3 dx2 dx1 dx0
//   byte 3: 0 0 dy5 dy4 dy3 dy2 dy1 dy0
//
// so the counts are 8-bit two's complement split between the two high bits in
// byte 1 and the six low bits of the following bytes, i.e. -128..+127. The
// classic mouse has two buttons; the middle one has no bit here. QEMU's
// hw/input/msmouse.c sends the same layout, and the probe's QEMU double run is
// what confirms it.
//
// The counts are in the same terms the PS/2 mouse's packet uses, i.e. the board
// hook's dx/dy straight through, so one host movement means the same thing to
// either device.
typedef struct MsMouseDevice {
  // One byte towards the serial port's receiver (Uart16550Receive).
  void (*tx)(void* ctx, uint8_t byte);
  void* tx_ctx;
  struct MsMouseState* st;  // private model state
} MsMouseDevice;

// Re-runnable, like the other devices' Inits.
void MsMouseInit(MsMouseDevice* d);
void MsMouseSetTxSink(MsMouseDevice* d, void (*tx)(void* ctx, uint8_t byte), void* ctx);
// One host event: movement in mouse counts plus the button state afterwards
// (bit 0 left, bit 1 right) — the board's pointer hook, minus the wheel.
void MsMouseEvent(MsMouseDevice* d, int dx, int dy, int buttons);

#endif
