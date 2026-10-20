#include <windows.h>

#include "host/host.h"

// Emulated time has two time bases and one offset, mirroring QEMU's clock
// split (include/qemu/timer.h):
//
//   HostTimerNow()     the emulated clock: the one every device measures its
//                      periods against (PIT counters, APIC timer, CLINT mtime,
//                      the VGA retrace). Host wall time by default; under
//                      -icount it stops following the wall clock and advances
//                      by a declared amount per retired instruction instead
//                      (QEMU: icount_get() installed as get_virtual_clock).
//   HostTimeRealtime() the host's own wall clock, for the few things that
//                      measure host time rather than emulated time: the
//                      MC146818 time-of-day, the window's repaint throttle, the
//                      input poll throttle. Neither -icount nor --skip-idle may
//                      move it — a guest reading the clock reads the host's, not
//                      the emulator's (QEMU QEMU_CLOCK_REALTIME: "should be
//                      used only for stuff which does not change the virtual
//                      machine state").
//   g_warp_us          the offset on the emulated clock: --skip-idle jumps a
//                      halted CPU to the next device deadline instead of
//                      letting the wait elapse in wall time (QEMU's virtual
//                      clock warp). HostTimerNow() reports its time base plus
//                      this, so a warp moves every device's deadline at once
//                      without touching the devices themselves.
static int64_t g_warp_us;

// -icount shift=N: log2 of the nanoseconds one retired instruction accounts for,
// or negative for the wall-clock time base. QEMU spells it the same way.
static int g_icount_shift = -1;
// Emulated nanoseconds since the first retired instruction. Derived, not
// accumulated from a per-step delta, so it is exactly inst_count << shift.
static int64_t g_icount_ns;

static int64_t HostWallUs(void) {
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

int64_t HostTimerNow(void) {
  if (g_icount_shift >= 0) return g_icount_ns / 1000 + g_warp_us;
  return HostWallUs() + g_warp_us;
}

int64_t HostTimeRealtime(void) { return HostWallUs(); }

void HostTimerIcountEnable(int shift) {
  g_icount_shift = shift;
  g_icount_ns = 0;
}

int HostTimerIcountOn(void) { return g_icount_shift >= 0; }

void HostTimerIcountAdvance(void) {
  if (g_icount_shift >= 0) g_icount_ns += (int64_t)1 << g_icount_shift;
}

void HostTimerWarp(int64_t us) {
  if (us > 0) g_warp_us += us;
}

// The run loop's last resort while the guest sleeps: it has reached no device
// deadline to jump to (only the x86 board can name one), and the sleep exists
// so a host key press can arrive. Under -icount the emulated clock moves only
// through retired instructions, so charging nothing here would leave a halted
// guest with a clock that never advances and an interrupt that never fires —
// so the wait is billed to the emulated clock at the length it was asked for.
// This is the one place where an -icount run stops being reproducible, and it
// is the same hole QEMU leaves: icount_start_warp_timer() accounts real time to
// the virtual clock while the CPUs are idle.
void HostSleepMs(int ms) {
  Sleep((DWORD)ms);
  if (g_icount_shift >= 0) g_icount_ns += (int64_t)ms * 1000;
}
