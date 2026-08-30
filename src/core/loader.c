#include <string.h>
#include <stdlib.h>
#include "core/loader.h"
#include "core/elf.h"
#include "host/host.h"
#include "util/log.h"

static int ReadWholeFile(const char *path, uint8_t **out, size_t *out_size) {
  HostFile *f = HostFileOpenRead(path);
  if (!f) {
    LogError("cannot open '%s'", path);
    return -1;
  }
  int64_t size = HostFileSize(f);
  if (size <= 0) {
    HostFileClose(f);
    LogError("empty or unreadable '%s'", path);
    return -1;
  }
  uint8_t *buf = (uint8_t *)malloc((size_t)size);
  if (!buf) {
    HostFileClose(f);
    return -1;
  }
  size_t got = HostFileRead(f, buf, (size_t)size);
  HostFileClose(f);
  if (got != (size_t)size) {
    free(buf);
    LogError("short read on '%s'", path);
    return -1;
  }
  *out = buf;
  *out_size = got;
  return 0;
}

// Raw bins have no e_machine to self-identify, so they fall back to this ISA.
static const char kDefaultIsaName[] = "riscv64";

// Raw-bin HTIF placement, from riscv-test-env p/riscv_test.h: tohost and
// fromhost are stored 64-byte-aligned, so fromhost lands 0x40 after tohost.
enum { kBinFromhostOffset = 0x40 };

static const IsaOps *FindIsaByMachine(uint32_t elf_machine) {
  for (const IsaOps *const *p = kIsaTable; *p; p++) {
    if ((*p)->elf_machine == elf_machine) return *p;
  }
  return NULL;
}

static const IsaOps *FindIsaByName(const char *name) {
  for (const IsaOps *const *p = kIsaTable; *p; p++) {
    if (strcmp((*p)->name, name) == 0) return *p;
  }
  return NULL;
}

// ELF images always self-identify through e_machine; --isa must agree if the
// user gave it. Raw bins have no header, so --isa (or the default) applies.
static const IsaOps *PickIsa(const char *isa_name, int is_elf,
                             uint32_t elf_machine) {
  if (is_elf) {
    const IsaOps *isa = FindIsaByMachine(elf_machine);
    if (!isa) {
      LogError("elf e_machine %u has no registered isa", elf_machine);
      return NULL;
    }
    if (isa_name && strcmp(isa_name, isa->name) != 0) {
      LogError("elf requires isa %s but --isa says %s", isa->name, isa_name);
      return NULL;
    }
    return isa;
  }
  const IsaOps *isa = FindIsaByName(isa_name ? isa_name : kDefaultIsaName);
  if (!isa) {
    LogError("unknown isa '%s'", isa_name);
    return NULL;
  }
  return isa;
}

int LoaderLoadImage(Machine *m, const char *path, const char *isa_name,
                    uint64_t bin_base, uint64_t bin_tohost, LoadResult *out) {
  memset(out, 0, sizeof(*out));
  uint8_t *data = NULL;
  size_t size = 0;
  if (ReadWholeFile(path, &data, &size) != 0) return -1;

  const IsaOps *isa = NULL;
  if (size >= 4 && memcmp(data, "\x7f" "ELF", 4) == 0) {
    ElfInfo info;
    if (ElfLoad(&m->bus, data, size, &info) != 0) {
      free(data);
      return -1;
    }
    isa = PickIsa(isa_name, 1, info.machine);
    if (!isa) {
      free(data);
      return -1;
    }
    out->entry = info.entry;
    if (info.has_tohost) {
      out->has_htif = 1;
      out->tohost = info.tohost;
      out->fromhost = info.fromhost;
    }
  } else {
    isa = PickIsa(isa_name, 0, 0);
    if (!isa) {
      free(data);
      return -1;
    }
    uint8_t *host = NULL;
    if (BusRamRange(&m->bus, bin_base, size, &host) != 0) {
      LogError("bin image of %llu bytes does not fit at %llx",
               (unsigned long long)size, (unsigned long long)bin_base);
      free(data);
      return -1;
    }
    memcpy(host, data, size);
    out->entry = bin_base;
    out->has_htif = 1;
    out->tohost = bin_tohost;
    out->fromhost = bin_tohost + kBinFromhostOffset;
  }
  free(data);
  out->isa = isa;

  if (out->has_htif) {
    HtifBind(&m->htif, &m->cpu);
    HtifRegister(&m->bus, &m->htif, out->tohost, out->fromhost);
  }
  return 0;
}
