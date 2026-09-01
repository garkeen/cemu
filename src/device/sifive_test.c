#include "device/sifive_test.h"
#include "util/log.h"

enum {
  kTestPass = 0x5555,
  kTestFail = 0x3333,
  kTestReset = 0x7777,
};

static uint64_t TestRead(void *dev, uint64_t addr, int size) {
  (void)dev;
  (void)addr;
  (void)size;
  return 0;
}

static void TestWrite(void *dev, uint64_t addr, int size, uint64_t val) {
  SifiveTestDevice *t = (SifiveTestDevice *)dev;
  (void)addr;
  (void)size;
  uint32_t code = (uint32_t)val & 0xffff;
  switch (code) {
    case kTestPass:
      t->cpu->halted = kCpuExited;
      t->cpu->exit_code = 0;
      break;
    case kTestFail:
      t->cpu->halted = kCpuExited;
      t->cpu->exit_code = (int)((val >> 16) & 0xffff);
      break;
    case kTestReset:
      LogInfo("sifive_test: reset requested");
      break;
    default:
      break;
  }
}

const DeviceOps kSifiveTestOps = {"sifive_test", TestRead, TestWrite};

void SifiveTestBind(SifiveTestDevice *dev, CpuState *cpu) {
  dev->cpu = cpu;
}

void SifiveTestRegister(Bus *bus, SifiveTestDevice *dev, uint64_t base,
                        uint64_t size) {
  BusAddRegion(bus, base, size, &kSifiveTestOps, dev);
}
