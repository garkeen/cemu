#include "device/misc/debugcon.h"

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
  char c = (char)(val & 0xff);
  HostWriteOut(&c, 1);
}

const DeviceOps kDebugConOps = {"debugcon", DebugConRead, DebugConWrite};

void DebugConRegister(Bus* io, uint16_t port) { BusAddRegion(io, port, 1, &kDebugConOps, NULL); }
