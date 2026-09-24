#ifndef CEMU_DEVICE_INPUT_PS2KBD_H
#define CEMU_DEVICE_INPUT_PS2KBD_H

#include <stdint.h>

// PS/2 keyboard: the device on the 8042's keyboard port, the counterpart of
// ps2mouse.c on the auxiliary port. Like the mouse it is a byte-stream device
// behind the controller — the controller hands it a command byte and reads its
// ACK, its replies and its scancodes back out of the same output queue.
//
// The device's native set is 2 (the AT's; aeb scancodes-10 §10.1) and the 8042
// translates set 2 into the set 1 a guest reads. Here the host already speaks
// set 1 (Win32 hands out set-1 scan codes), so the same table runs in the other
// direction: the device stores the set the guest selected plus the controller's
// translation bit and picks what to emit —
//
//   translation on (the default) or set 1 -> the host's set-1 bytes, unchanged
//   translation off and set 2             -> the set-2 equivalents
//
// which is the observable half of what a keyboard plus a real 8042 does. Aeb:
// "Set 1 should not be translated, while sets 2 and 3 should be translated"
// (§10.1), so a guest in set 1 gets its bytes through whatever the bit says.
//
// Sets 1 and 2 are the ones a PC/AT uses; this model does not claim set 3, and
// the commands that only mean something in set 3 (0xf7-0xfd, which set a key's
// repeat and break bits) are accepted and have no effect — aeb §12.1: "It does
// not influence keyboard operation when the scancode set is not Set 3".
typedef struct Ps2KbdDevice {
  // The device's byte stream towards the controller's keyboard output queue.
  void (*tx)(void* ctx, uint8_t byte);
  void* tx_ctx;
  struct Ps2KbdState* st;  // private model state
} Ps2KbdDevice;

// Re-runnable: the board's reset path calls it again, so it must leave the
// sink installed (the same contract as I8042Init and Ps2MouseInit).
void Ps2KbdInit(Ps2KbdDevice* d);
void Ps2KbdSetTxSink(Ps2KbdDevice* d, void (*tx)(void* ctx, uint8_t byte), void* ctx);
// A byte the controller routed to the keyboard port: a command, or the
// parameter byte of one.
void Ps2KbdWrite(Ps2KbdDevice* d, uint8_t val);
// The controller's translation bit — command-byte bit 6 (PC/AT Technical
// Reference; aeb §10.3 calls it "scan code conversion to PC format").
void Ps2KbdSetTranslate(Ps2KbdDevice* d, int on);
// One host key: the set-1 make code the host reports, with `extended` for the
// 0xe0-prefixed keys and `up` for a release — the triple
// HostDisplaySetKeySink hands out.
void Ps2KbdKey(Ps2KbdDevice* d, uint32_t scan, int extended, int up);

#endif
