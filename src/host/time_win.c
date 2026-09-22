#include <windows.h>

#include "host/host.h"

// Emulated-time offset (--skip-idle): HostTimerNow() reports host time plus
// this, so a warp moves every device's deadline at once without touching the
// devices themselves.
static int64_t g_warp_us;

int64_t HostTimerNow(void) {
  static LARGE_INTEGER freq;
  static int inited = 0;
  LARGE_INTEGER now;
  if (!inited) {
    QueryPerformanceFrequency(&freq);
    inited = 1;
  }
  QueryPerformanceCounter(&now);
  return (int64_t)(now.QuadPart * 1000000 / freq.QuadPart) + g_warp_us;
}

void HostTimerWarp(int64_t us) {
  if (us > 0) g_warp_us += us;
}

void HostSleepMs(int ms) { Sleep((DWORD)ms); }
