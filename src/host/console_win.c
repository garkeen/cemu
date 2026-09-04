#include <windows.h>

#include "host/host.h"

static void WriteTo(DWORD std_handle, const char* buf, size_t n) {
  HANDLE h = GetStdHandle(std_handle);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  while (n > 0) {
    if (!WriteFile(h, buf, (DWORD)n, &written, NULL)) return;
    if (written == 0) return;
    buf += written;
    n -= written;
  }
}

void HostWriteOut(const char* buf, size_t n) { WriteTo(STD_OUTPUT_HANDLE, buf, n); }

void HostWriteErr(const char* buf, size_t n) { WriteTo(STD_ERROR_HANDLE, buf, n); }
