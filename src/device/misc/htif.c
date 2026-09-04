#include "device/misc/htif.h"

#include <string.h>

#include "host/host.h"
#include "util/log.h"

static int IsTohost(HtifDevice* h, uint64_t addr) { return addr - h->tohost_addr < 8; }

static uint64_t HtifRead(void* dev, uint64_t addr, int size) {
  HtifDevice* h = (HtifDevice*)dev;
  uint64_t base = IsTohost(h, addr) ? h->tohost : h->fromhost;
  uint64_t val = 0;
  memcpy((uint8_t*)&val, (uint8_t*)&base + (addr & 7), (size_t)size);
  return val;
}

static void HtifProcess(HtifDevice* h, uint64_t val) {
  uint32_t dev = (uint32_t)(val >> 56);
  uint32_t cmd = (uint32_t)((val >> 48) & 0xff);
  uint64_t payload = val & 0xffffffffffffULL;
  if (dev == 0 && cmd == 0) {
    if (payload & 1) {
      h->cpu->halted = kCpuExited;
      h->cpu->exit_code = (int)(payload >> 1);
    } else {
      LogError("htif: syscall payload %llx not supported", (unsigned long long)payload);
      h->cpu->halted = kCpuExited;
      h->cpu->exit_code = 1;
    }
    return;
  }
  if (dev == 1 && cmd == 1) {
    char ch = (char)(payload & 0xff);
    HostWriteOut(&ch, 1);
  } else {
    LogError("htif: unhandled dev=%u cmd=%u payload=%llx", dev, cmd, (unsigned long long)payload);
    h->cpu->halted = kCpuExited;
    h->cpu->exit_code = 1;
  }
}

static void HtifWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  HtifDevice* h = (HtifDevice*)dev;
  if (!IsTohost(h, addr)) {
    // fromhost: the guest consumes the ack by writing (usually clearing) it
    uint64_t v = h->fromhost;
    memcpy((uint8_t*)&v + (addr & 7), &val, (size_t)size);
    h->fromhost = v;
    return;
  }
  // fesvr contract: the host polls tohost, so any non-zero assembled value is
  // a complete command — a full 64-bit store for device commands, or the
  // riscv-tests exit pair whose low word already carries (code << 1) | 1.
  // Assemble the written bytes, then fire on non-zero (single path).
  uint64_t v = h->tohost;
  memcpy((uint8_t*)&v + (addr & 7), &val, (size_t)size);
  h->tohost = v;
  if (v) {
    HtifProcess(h, v);
    h->tohost = 0;
    // ack with the same dev/cmd, as fesvr/dearchap-tinyemu do; an exit halts
    // the hart so no ack is needed.
    if (!h->cpu->halted) h->fromhost = ((v >> 56) << 56) | (((v >> 48) & 0xff) << 48);
  }
}

const DeviceOps kHtifOps = {"htif", HtifRead, HtifWrite};

void HtifBind(HtifDevice* htif, CpuState* cpu) { htif->cpu = cpu; }

void HtifRegister(Bus* bus, HtifDevice* htif, uint64_t tohost_addr, uint64_t fromhost_addr) {
  htif->tohost_addr = tohost_addr;
  htif->fromhost_addr = fromhost_addr;
  BusAddRegion(bus, tohost_addr, 8, &kHtifOps, htif);
  BusAddRegion(bus, fromhost_addr, 8, &kHtifOps, htif);
}
