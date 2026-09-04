#ifndef CEMU_BOARD_LOADER_H
#define CEMU_BOARD_LOADER_H

#include <stdint.h>

#include "bus/bus.h"
// isa_ops and the ISA registry live in the CPU socket's step contract, so
// picking a CPU model needs no cpu/isa/ header.
#include "cpu/step.h"

typedef struct LoadResult {
  const isa_ops* isa;
  uint64_t entry;
  uint64_t image_base;  // where the image starts in memory
  int has_htif;
  uint64_t tohost;
  uint64_t fromhost;
} LoadResult;

// Loads .elf (segments + symbols) or .bin (raw image at bin_base).
// isa_name picks the ISA for raw bins; ELF files self-identify.
// bin_uses_htif is a machine property (spike yes, virt/PC no): it selects
// the raw-binary HTIF convention; ELFs always decide by their symbols.
// HTIF placement is reported, not wired; the caller attaches the device.
int LoaderLoadImage(Bus* bus, const char* path, const char* isa_name, uint64_t bin_base,
                    uint64_t bin_tohost, int bin_uses_htif, LoadResult* out);

#endif
