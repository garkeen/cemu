#include "device/input/msmouse.h"

#include <stdlib.h>
#include <string.h>

struct MsMouseState {
  int buttons;  // bit 0 left, bit 1 right
  int dx, dy;   // movement the host has reported and the guest has not read
};

static void Tx(MsMouseDevice* d, uint8_t b) {
  if (d->tx) d->tx(d->tx_ctx, b);
}

// One packet from the accumulated movement and the current buttons. The counts
// are 8-bit two's complement: the top two bits ride in byte 1 and the low six in
// the byte after it, so a movement outside -128..+127 is clamped (QEMU's
// msmouse clamps the same way). A negative count's high bits come out of the
// shift as 11, which is exactly the sign extension the receiver undoes.
static void SendPacket(MsMouseDevice* d) {
  struct MsMouseState* st = d->st;
  int dx = st->dx, dy = st->dy;
  if (dx < -128)
    dx = -128;
  else if (dx > 127)
    dx = 127;
  if (dy < -128)
    dy = -128;
  else if (dy > 127)
    dy = 127;
  uint8_t b0 = 0x40;  // bit 6 is set on every first byte
  if (st->buttons & 0x01) b0 |= 0x20;  // left
  if (st->buttons & 0x02) b0 |= 0x10;  // right
  b0 |= (uint8_t)(((dy >> 6) & 0x03) << 2);
  b0 |= (uint8_t)((dx >> 6) & 0x03);
  Tx(d, b0);
  Tx(d, (uint8_t)(dx & 0x3f));
  Tx(d, (uint8_t)(dy & 0x3f));
  st->dx = 0;
  st->dy = 0;
}

void MsMouseEvent(MsMouseDevice* d, int dx, int dy, int buttons) {
  struct MsMouseState* st = d->st;
  int buttons_new = buttons & 0x03;
  // The device has no enable bit and no poll: it reports every change, a button
  // with no movement included, and stays quiet when nothing changed.
  if (buttons_new == st->buttons && !dx && !dy) return;
  st->buttons = buttons_new;
  st->dx += dx;
  st->dy += dy;
  SendPacket(d);
}

void MsMouseSetTxSink(MsMouseDevice* d, void (*tx)(void* ctx, uint8_t byte), void* ctx) {
  d->tx = tx;
  d->tx_ctx = ctx;
}

void MsMouseInit(MsMouseDevice* d) {
  if (!d->st) {
    d->st = (struct MsMouseState*)calloc(1, sizeof(struct MsMouseState));
    if (!d->st) return;
  } else {
    memset(d->st, 0, sizeof(*d->st));
  }
  d->tx = NULL;
  d->tx_ctx = NULL;
}
