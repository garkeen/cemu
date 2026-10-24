#include <windows.h>

#include <stdlib.h>

#include "host/host.h"

// The system clipboard, UTF-8 text only — the same contract SDL's
// SDL_GetClipboardText / SDL_SetClipboardText give the reference emulator's
// clipboard channel. windows.h lives only in this directory (AGENTS.md §七).
//
// OpenClipboard can fail while another window is still draining its paste
// menu; the customary remedy is a short retry, which is what SDL's win32
// backend does too.

enum { kClipOpenTries = 8, kClipOpenRetryMs = 10 };

static int OpenClipboardSoon(void) {
  for (int i = 0; i < kClipOpenTries; i++) {
    if (OpenClipboard(NULL)) return 1;
    Sleep(kClipOpenRetryMs);
  }
  return 0;
}

char* HostClipboardGetText(void) {
  if (!OpenClipboardSoon()) return NULL;
  char* out = NULL;
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  if (h) {
    const WCHAR* wtext = (const WCHAR*)GlobalLock(h);
    if (wtext) {
      int bytes = WideCharToMultiByte(CP_UTF8, 0, wtext, -1, NULL, 0, NULL, NULL);
      if (bytes > 0) {
        out = (char*)malloc((size_t)bytes);
        if (out) WideCharToMultiByte(CP_UTF8, 0, wtext, -1, out, bytes, NULL, NULL);
      }
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return out;  // NULL = no text on the clipboard (or busy); caller frees
}

int HostClipboardSetText(const char* text) {
  if (!OpenClipboardSoon()) return -1;
  int ok = -1;
  int wchars = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
  if (wchars > 0) {
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wchars * sizeof(WCHAR));
    if (mem) {
      WCHAR* wtext = (WCHAR*)GlobalLock(mem);
      if (wtext) {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, wchars);
        GlobalUnlock(mem);
        if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, mem)) {
          ok = 0;
          mem = NULL;  // the clipboard owns it now
        }
      }
      if (mem) GlobalFree(mem);
    }
  }
  CloseClipboard();
  return ok;
}
