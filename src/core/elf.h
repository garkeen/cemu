#ifndef CEMU_CORE_ELF_H
#define CEMU_CORE_ELF_H

#include <stddef.h>
#include <stdint.h>
#include "core/bus.h"

typedef struct ElfInfo {
  uint64_t entry;
  uint64_t image_base;  // lowest PT_LOAD address
  uint32_t machine;  // e_machine, matched against IsaOps.elf_machine
  int is_64;
  int has_tohost;
  uint64_t tohost;
  uint64_t fromhost;
} ElfInfo;

// Parses the image, loads PT_LOAD segments into RAM regions on the bus and
// extracts the tohost/fromhost symbols if a symbol table is present.
// Returns 0 on success.
int ElfLoad(Bus *bus, const uint8_t *data, size_t size, ElfInfo *out);

#endif
