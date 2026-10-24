#ifndef CEMU_DEVICE_MISC_OBERON_CLIPBOARD_H
#define CEMU_DEVICE_MISC_OBERON_CLIPBOARD_H

#include <stdint.h>

// The Oberon machine's clipboard channel, ported from the reference
// emulator's sdl-clipboard.c: a two-register GET/PUT protocol between the
// guest and the host system clipboard. A control *read* fetches the host
// clipboard and returns the byte length the guest may then pull byte by byte
// from the data register; a control *write* of a length switches to PUT, and
// after that many data writes the collected text lands on the host clipboard.
// Line endings fold both ways: the guest's CR becomes the host's LF, and a
// host CRLF pair counts — and delivers — as one CR.
typedef struct OberonClipboardDevice {
  enum OberonClipState { kClipIdle, kClipGet, kClipPut } state;
  char* data;
  uint32_t ptr;
  uint32_t len;
} OberonClipboardDevice;

void OberonClipboardInit(OberonClipboardDevice* d);
void OberonClipboardDestroy(OberonClipboardDevice* d);
uint32_t OberonClipboardReadControl(OberonClipboardDevice* d);
void OberonClipboardWriteControl(OberonClipboardDevice* d, uint32_t len);
uint32_t OberonClipboardReadData(OberonClipboardDevice* d);
void OberonClipboardWriteData(OberonClipboardDevice* d, uint32_t c);

#endif  // CEMU_DEVICE_MISC_OBERON_CLIPBOARD_H
