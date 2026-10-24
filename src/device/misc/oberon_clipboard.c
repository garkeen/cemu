#include "device/misc/oberon_clipboard.h"

#include <stdlib.h>
#include <string.h>

#include "host/host.h"

static void ClipReset(OberonClipboardDevice* d) {
  d->state = kClipIdle;
  free(d->data);
  d->data = NULL;
  d->len = 0;
  d->ptr = 0;
}

void OberonClipboardInit(OberonClipboardDevice* d) {
  memset(d, 0, sizeof(*d));
}

void OberonClipboardDestroy(OberonClipboardDevice* d) { ClipReset(d); }

// Control read: fetch the host clipboard and report the folded length
// (sdl-clipboard.c clipboard_control_read).
uint32_t OberonClipboardReadControl(OberonClipboardDevice* d) {
  uint32_t r = 0;
  ClipReset(d);
  d->data = HostClipboardGetText();
  if (d->data) {
    d->len = (uint32_t)strlen(d->data);
    if (d->len > 0) {
      d->state = kClipGet;
      r = d->len;
      // Every CR LF pair will deliver as one byte.
      for (const char* p = d->data; (p = strchr(p, '\r')) != NULL; p++) {
        if (p[1] == '\n') r--;
      }
    }
  }
  return r;
}

void OberonClipboardWriteControl(OberonClipboardDevice* d, uint32_t len) {
  ClipReset(d);
  d->data = (char*)malloc((size_t)len + 1);
  if (d->data) {
    d->len = len;
    d->state = kClipPut;
  }
}

uint32_t OberonClipboardReadData(OberonClipboardDevice* d) {
  uint32_t result = 0;
  if (d->state == kClipGet && d->data && d->ptr < d->len) {
    result = (uint8_t)d->data[d->ptr];
    d->ptr++;
    if (result == '\r' && d->ptr < d->len && d->data[d->ptr] == '\n') {
      d->ptr++;  // the LF of a CRLF pair rides along with its CR
    } else if (result == '\n') {
      result = '\r';
    }
    if (d->ptr >= d->len) ClipReset(d);
  }
  return result;
}

void OberonClipboardWriteData(OberonClipboardDevice* d, uint32_t c) {
  if (d->state == kClipPut && d->data && d->ptr < d->len) {
    if (c == '\r') c = '\n';
    d->data[d->ptr++] = (char)c;
    if (d->ptr == d->len) {
      d->data[d->len] = 0;
      HostClipboardSetText(d->data);
      ClipReset(d);
    }
  }
}
