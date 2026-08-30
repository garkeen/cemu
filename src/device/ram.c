#include <stdlib.h>
#include <string.h>
#include "device/ram.h"

static uint64_t RamRead(void *dev, uint64_t addr, int size) {
  RamDevice *ram = (RamDevice *)dev;
  uint64_t off = addr - ram->base;
  uint64_t val = 0;
  memcpy(&val, ram->mem + off, (size_t)size);
  return val;
}

static void RamWrite(void *dev, uint64_t addr, int size, uint64_t val) {
  RamDevice *ram = (RamDevice *)dev;
  uint64_t off = addr - ram->base;
  memcpy(ram->mem + off, &val, (size_t)size);
}

const DeviceOps kRamOps = {"ram", RamRead, RamWrite};

RamDevice *RamCreate(uint64_t base, uint64_t size) {
  RamDevice *ram = (RamDevice *)malloc(sizeof(RamDevice));
  if (!ram) return NULL;
  ram->mem = (uint8_t *)malloc((size_t)size);
  if (!ram->mem) {
    free(ram);
    return NULL;
  }
  memset(ram->mem, 0, (size_t)size);
  ram->base = base;
  ram->size = size;
  return ram;
}

void RamDestroy(RamDevice *ram) {
  if (!ram) return;
  free(ram->mem);
  free(ram);
}
