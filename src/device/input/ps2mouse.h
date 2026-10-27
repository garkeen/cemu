#ifndef CEMU_DEVICE_INPUT_PS2MOUSE_H
#define CEMU_DEVICE_INPUT_PS2MOUSE_H

#include <stdint.h>

// PS/2 mouse: the device on the 8042's auxiliary port (the "AUX" or "mouse"
// channel). It is a byte-stream device like the keyboard — the controller
// writes a command byte to it and reads its ACK, its replies and its data
// packets back out of the same output queue — so it hangs off the controller
// through a byte sink pair, not off the bus:
//
//   controller --0xd4 + data--> Ps2MouseWrite --> tx sink --> controller's AUX queue
//
// The packet is the standard 3-byte one (PS/2 mouse protocol): byte 1 carries
// Yovfl Xovfl dy8 dx8 1 Middle Right Left, then the low 8 bits of dx and dy.
// The counts are 9-bit two's complement, i.e. -256..+255; a count outside that
// range sets the matching overflow bit. A wheel mouse (ID 0x03, the "IMPS/2"
// that the sample-rate handshake below announces) appends a 4th byte with the
// wheel movement.
//
// Movement and buttons come from the host — the display window's mouse
// (HostDisplaySetMouseSink) or the debug hub's synthetic events — through
// Ps2MouseEvent, which is the device's only host-facing entry point.
typedef struct Ps2MouseDevice {
  // The device's byte stream towards the controller's output queue.
  void (*tx)(void* ctx, uint8_t byte);
  void* tx_ctx;
  struct Ps2MouseState* st;  // private model state
} Ps2MouseDevice;

// Re-runnable: the board's reset path calls it again, so it must leave the
// sinks installed (the same contract as I8042Init).
void Ps2MouseInit(Ps2MouseDevice* d);
void Ps2MouseSetTxSink(Ps2MouseDevice* d, void (*tx)(void* ctx, uint8_t byte), void* ctx);
// A byte the controller routed to the AUX port (command 0xd4's payload).
void Ps2MouseWrite(Ps2MouseDevice* d, uint8_t val);
// One host event: movement (dx/dy in mouse counts, dz the wheel) plus the
// button state afterwards — bit 0 left, bit 1 right, bit 2 middle, the same
// bit order the packet's first byte uses (ps2.js mouse_send_click).
void Ps2MouseEvent(Ps2MouseDevice* d, int dx, int dy, int dz, int buttons);
// Whether the device emits a packet right now: reporting on (0xf4) and not in
// remote mode. The board's absolute pointer integration uses this so it never
// advances a guest-cursor tracker with packets the guest will not see.
int Ps2MouseCanReport(const Ps2MouseDevice* d);
// 2:1 scaling (0xe7) transforms counts before they reach the packet, so a
// delta tracker cannot assume 1:1.
int Ps2MouseScaling(const Ps2MouseDevice* d);
// The device's 2:1 transform and an inverse that returns a count whose scaled
// result is as close as possible to the requested movement. The board's
// absolute model uses them so the guest cursor still reaches the target when
// the driver turns scaling on.
int Ps2MouseScale21(int n);
int Ps2MouseScale21Inverse(int d);

#endif
