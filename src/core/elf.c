#include <string.h>
#include "core/elf.h"
#include "util/log.h"

enum {
  kPtLoad = 1,
  kShtSymtab = 2,
};

static uint16_t Rd16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t Rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t Rd64(const uint8_t *p) {
  return (uint64_t)Rd32(p) | ((uint64_t)Rd32(p + 4) << 32);
}

static int NameIs(const uint8_t *strtab, uint32_t off, const char *name) {
  return strcmp((const char *)strtab + off, name) == 0;
}

static void FindHtifSymbols(const uint8_t *data, size_t size,
                            uint64_t shoff, uint16_t shentsize,
                            uint16_t shnum, uint32_t shstrndx, ElfInfo *out) {
  if (shoff == 0 || shnum == 0 || shoff + (uint64_t)shnum * shentsize > size)
    return;
  // section header string table gives us .symtab and .strtab names
  const uint8_t *shstr_hdr = data + shoff + (size_t)shstrndx * shentsize;
  uint64_t shstr_off = Rd64(shstr_hdr + 24);
  uint64_t shstr_size = Rd64(shstr_hdr + 32);
  if (shstr_off + shstr_size > size) return;
  const uint8_t *shstr = data + shstr_off;

  const uint8_t *symtab = NULL;
  const uint8_t *strtab = NULL;
  uint64_t sym_count = 0;
  for (uint16_t i = 0; i < shnum; i++) {
    const uint8_t *sh = data + shoff + (size_t)i * shentsize;
    uint32_t type = Rd32(sh + 4);
    uint32_t name_off = Rd32(sh);
    uint64_t offset = Rd64(sh + 24);
    uint64_t shsize = Rd64(sh + 32);
    uint64_t entsize = Rd64(sh + 56);
    const char *sname = (const char *)shstr + name_off;
    if (type == kShtSymtab && strcmp(sname, ".symtab") == 0) {
      symtab = data + offset;
      sym_count = entsize ? shsize / entsize : 0;
      // the linked section is the string table for symbols
      const uint8_t *str_hdr = data + shoff + (size_t)Rd32(sh + 40) * shentsize;
      strtab = data + Rd64(str_hdr + 24);
    }
  }
  if (!symtab || !strtab) return;
  for (uint64_t i = 0; i < sym_count; i++) {
    const uint8_t *sym = symtab + i * 24;  // Elf64_Sym is 24 bytes
    uint32_t name_off = Rd32(sym);
    uint64_t value = Rd64(sym + 8);
    if (name_off == 0) continue;
    if (NameIs(strtab, name_off, "tohost")) {
      out->tohost = value;
      out->has_tohost = 1;
    } else if (NameIs(strtab, name_off, "fromhost")) {
      out->fromhost = value;
    }
  }
}

int ElfLoad(Bus *bus, const uint8_t *data, size_t size, ElfInfo *out) {
  memset(out, 0, sizeof(*out));
  if (size < 64 || memcmp(data, "\x7f" "ELF", 4) != 0) return -1;
  uint8_t ei_class = data[4];
  if (ei_class != 1 && ei_class != 2) return -1;
  int is64 = (ei_class == 2);
  out->is_64 = is64;
  out->machine = Rd16(data + 18);

  uint64_t phoff, shoff;
  uint16_t phentsize, phnum, shentsize, shnum, shstrndx;
  if (is64) {
    out->entry = Rd64(data + 24);
    phoff = Rd64(data + 32);
    shoff = Rd64(data + 40);
    phentsize = Rd16(data + 54);
    phnum = Rd16(data + 56);
    shentsize = Rd16(data + 58);
    shnum = Rd16(data + 60);
    shstrndx = Rd16(data + 62);
  } else {
    out->entry = Rd32(data + 24);
    phoff = Rd32(data + 28);
    shoff = Rd32(data + 32);
    phentsize = Rd16(data + 42);
    phnum = Rd16(data + 44);
    shentsize = Rd16(data + 46);
    shnum = Rd16(data + 48);
    shstrndx = Rd16(data + 50);
  }

  for (uint16_t i = 0; i < phnum; i++) {
    const uint8_t *ph = data + phoff + (size_t)i * phentsize;
    uint32_t p_type;
    uint64_t p_offset, p_vaddr, p_filesz, p_memsz;
    if (is64) {
      p_type = Rd32(ph);
      p_offset = Rd64(ph + 8);
      p_vaddr = Rd64(ph + 16);
      p_filesz = Rd64(ph + 32);
      p_memsz = Rd64(ph + 40);
    } else {
      p_type = Rd32(ph);
      p_offset = Rd32(ph + 4);
      p_vaddr = Rd32(ph + 8);
      p_filesz = Rd32(ph + 16);
      p_memsz = Rd32(ph + 20);
    }
    if (p_type != kPtLoad) continue;
    if (!out->image_base || p_vaddr < out->image_base)
      out->image_base = p_vaddr;
    uint8_t *host = NULL;
    if (BusRamRange(bus, p_vaddr, p_memsz, &host) != 0) {
      LogError("elf: segment [%llx, %llx) outside RAM",
               (unsigned long long)p_vaddr,
               (unsigned long long)(p_vaddr + p_memsz));
      return -1;
    }
    if (p_offset + p_filesz > size) return -1;
    memcpy(host, data + p_offset, (size_t)p_filesz);
    if (p_memsz > p_filesz) memset(host + p_filesz, 0, (size_t)(p_memsz - p_filesz));
  }

  if (is64)
    FindHtifSymbols(data, size, shoff, shentsize, shnum, shstrndx, out);
  return 0;
}
