#include "device/misc/debugcon.h"

#include "debug/debug.h"
#include "host/host.h"

// The value firmware reads back to detect the port (seabios paravirt.h).
enum { kDebugConReadback = 0xe9 };

static uint64_t DebugConRead(void* dev, uint64_t addr, int size) {
  (void)dev;
  (void)addr;
  (void)size;
  return kDebugConReadback;
}

static void DebugConWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)dev;
  (void)addr;
  (void)size;
  // The guest's own console: every byte here came from the guest, so a row per
  // byte (with the PC and the instruction ring, see debug.h) is what turns
  // "the console shows garbage" into "this instruction read that string".
  DebugMark("debugcon-tx", (int)(val & 0xff), 0);
  char c = (char)(val & 0xff);
  HostWriteOut(&c, 1);
}

const DeviceOps kDebugConOps = {"debugcon", DebugConRead, DebugConWrite};

void DebugConRegister(Bus* io, uint16_t port) { BusAddRegion(io, port, 1, &kDebugConOps, NULL); }
