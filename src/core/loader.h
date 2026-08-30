#ifndef CEMU_CORE_LOADER_H
#define CEMU_CORE_LOADER_H

#include "core/machine.h"

typedef struct LoadResult {
  const IsaOps *isa;
  uint64_t entry;
  int has_htif;
  uint64_t tohost;
  uint64_t fromhost;
} LoadResult;

// Loads .elf (segments + symbols) or .bin (raw image at bin_base).
// isa_name picks the ISA for raw bins; ELF files self-identify.
int LoaderLoadImage(Machine *m, const char *path, const char *isa_name,
                    uint64_t bin_base, uint64_t bin_tohost, LoadResult *out);

#endif
