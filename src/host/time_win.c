#include <windows.h>
#include "host/host.h"

int64_t HostTimerNow(void) {
  static LARGE_INTEGER freq;
  static int inited = 0;
  LARGE_INTEGER now;
  if (!inited) {
    QueryPerformanceFrequency(&freq);
    inited = 1;
  }
  QueryPerformanceCounter(&now);
  return (int64_t)(now.QuadPart * 1000000 / freq.QuadPart);
}
