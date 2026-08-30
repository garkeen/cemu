#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/loader.h"
#include "core/machine.h"
#include "host/host.h"
#include "util/log.h"

typedef struct Args {
  const char *image;
  const char *isa_name;
  const char *machine_name;
  const char *log_file;
  uint64_t mem_size;
  uint64_t mem_base;
  uint64_t bin_base;
  uint64_t bin_tohost;
  uint64_t max_inst;
  int dump_regs;
} Args;

static void Usage(void) {
  static const char msg[] =
      "usage: cemu [options] image.bin|image.elf\n"
      "  --isa NAME        isa for raw bins (default riscv64); ELF files\n"
      "                    self-identify and --isa must agree if given\n"
      "  --machine NAME    machine model (default spike)\n"
      "  --mem MB          ram size in MB (default 256)\n"
      "  --base ADDR       ram base (default 0x80000000)\n"
      "  --bin-base ADDR   load address for raw bins (default ram base)\n"
      "  --htif ADDR       tohost address for raw bins (default base+0x1000)\n"
      "  --max-inst N      stop after N instructions (default unlimited)\n"
      "  --log FILE        also write logs to FILE\n"
      "  --dump-regs       dump registers on any exit\n";
  HostWriteErr(msg, sizeof(msg) - 1);
}

static int ParseU64(const char *s, uint64_t *out) {
  if (!s) return -1;
  *out = (uint64_t)strtoull(s, NULL, 0);
  return 0;
}

static int ParseArgs(Args *a, int argc, char **argv) {
  memset(a, 0, sizeof(*a));
  // NULL means "ELF self-identifies; raw bins fall back to riscv64"
  a->isa_name = NULL;
  a->machine_name = "spike";
  a->mem_size = kSpikeDramSizeDefault;
  a->mem_base = kSpikeDramBase;
  a->bin_base = kSpikeDramBase;
  a->bin_tohost = 0;  // after parsing: bin_base + kRawBinTohostOffset
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (strcmp(arg, "--isa") == 0) a->isa_name = argv[++i];
    else if (strcmp(arg, "--machine") == 0) a->machine_name = argv[++i];
    else if (strcmp(arg, "--log") == 0) a->log_file = argv[++i];
    else if (strcmp(arg, "--mem") == 0) {
      if (ParseU64(argv[++i], &a->mem_size)) return -1;
      a->mem_size <<= 20;
    } else if (strcmp(arg, "--base") == 0) {
      if (ParseU64(argv[++i], &a->mem_base)) return -1;
      a->bin_base = a->mem_base;
    } else if (strcmp(arg, "--bin-base") == 0) {
      if (ParseU64(argv[++i], &a->bin_base)) return -1;
    } else if (strcmp(arg, "--htif") == 0) {
      if (ParseU64(argv[++i], &a->bin_tohost)) return -1;
    } else if (strcmp(arg, "--max-inst") == 0) {
      if (ParseU64(argv[++i], &a->max_inst)) return -1;
    } else if (strcmp(arg, "--dump-regs") == 0) a->dump_regs = 1;
    else if (arg[0] == '-' && arg[1] == '-') return -1;
    else a->image = arg;
  }
  if (!a->image) return -1;
  if (!a->bin_tohost) a->bin_tohost = a->bin_base + kRawBinTohostOffset;
  return 0;
}

int main(int argc, char **argv) {
  Args a;
  if (ParseArgs(&a, argc, argv) != 0) {
    Usage();
    return 1;
  }
  if (a.log_file) LogInitFile(a.log_file);

  Machine *m = NULL;
  if (strcmp(a.machine_name, "spike") == 0) {
    m = MachineCreateSpike(a.mem_size, a.mem_base);
  } else {
    LogError("unknown machine '%s'", a.machine_name);
    return 1;
  }
  if (!m) {
    LogError("machine creation failed");
    return 1;
  }

  LoadResult lr;
  if (LoaderLoadImage(m, a.image, a.isa_name, a.bin_base, a.bin_tohost,
                      &lr) != 0) {
    MachineDestroy(m);
    return 1;
  }
  m->isa = lr.isa;
  m->cpu.pc = lr.entry;
  lr.isa->Init(&m->cpu);
  LogInfo("loaded %s: entry=%llx isa=%s htif=%d", a.image,
          (unsigned long long)lr.entry, lr.isa->name, lr.has_htif);

  MachineRun(m, a.max_inst);

  int code = m->cpu.exit_code;
  LogInfo("stopped: inst=%llu pc=%llx exit=%d",
          (unsigned long long)m->cpu.inst_count,
          (unsigned long long)m->cpu.pc, code);
  if (code != 0 || a.dump_regs) lr.isa->DumpRegs(&m->cpu);

  MachineDestroy(m);
  return code & 0xff;
}
