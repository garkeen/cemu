#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board/board.h"
#include "board/loader.h"
#include "host/host.h"
#include "util/log.h"

typedef struct Args {
  const char* image;
  const char* isa_name;
  const char* machine_name;
  const char* log_file;
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
      "  --mem MB          ram size in MB (default: machine's)\n"
      "  --base ADDR       ram base (default: machine's)\n"
      "  --bin-base ADDR   load address for raw bins (default: machine's)\n"
      "  --htif ADDR       tohost address for raw bins (default base+0x1000)\n"
      "  --max-inst N      stop after N instructions (default unlimited)\n"
      "  --log FILE        also write logs to FILE\n"
      "  --dump-regs       dump registers on any exit\n";
  HostWriteErr(msg, sizeof(msg) - 1);
}

static int ParseU64(const char* s, uint64_t* out) {
  if (!s) return -1;
  *out = (uint64_t)strtoull(s, NULL, 0);
  return 0;
}

// Memory layout knobs are 0 when unset; the machine applies its own defaults.
static int ParseArgs(Args* a, int argc, char** argv) {
  memset(a, 0, sizeof(*a));
  a->isa_name = NULL;
  a->machine_name = "spike";
  for (int i = 1; i < argc; i++) {
    const char* arg = argv[i];
    if (strcmp(arg, "--isa") == 0)
      a->isa_name = argv[++i];
    else if (strcmp(arg, "--machine") == 0)
      a->machine_name = argv[++i];
    else if (strcmp(arg, "--log") == 0)
      a->log_file = argv[++i];
    else if (strcmp(arg, "--mem") == 0) {
      if (ParseU64(argv[++i], &a->mem_size)) return -1;
      a->mem_size <<= 20;
    } else if (strcmp(arg, "--base") == 0) {
      if (ParseU64(argv[++i], &a->mem_base)) return -1;
    } else if (strcmp(arg, "--bin-base") == 0) {
      if (ParseU64(argv[++i], &a->bin_base)) return -1;
    } else if (strcmp(arg, "--htif") == 0) {
      if (ParseU64(argv[++i], &a->bin_tohost)) return -1;
    } else if (strcmp(arg, "--max-inst") == 0) {
      if (ParseU64(argv[++i], &a->max_inst)) return -1;
    } else if (strcmp(arg, "--dump-regs") == 0)
      a->dump_regs = 1;
    else if (arg[0] == '-' && arg[1] == '-')
      return -1;
    else
      a->image = arg;
  }
  if (!a->image) return -1;
  return 0;
}

int main(int argc, char** argv) {
  Args a;
  if (ParseArgs(&a, argc, argv) != 0) {
    Usage();
    return 1;
  }
  if (a.log_file) LogInitFile(a.log_file);

  BoardOpts opts = {a.mem_base, a.mem_size};
  Board* m = BoardCreate(a.machine_name, &opts);
  if (!m) return 1;

  uint64_t bin_base = a.bin_base ? a.bin_base : m->bin_base;
  LoadResult lr;
  if (LoaderLoadImage(&m->bus, a.image, a.isa_name, bin_base, a.bin_tohost, m->bin_htif, &lr) !=
      0) {
    BoardDestroy(m);
    return 1;
  }
  if (lr.has_htif) {
    HtifBind(&m->htif, &m->cpu);
    HtifRegister(&m->bus, &m->htif, lr.tohost, lr.fromhost);
  }
  m->isa = lr.isa;
  m->cpu.pc = m->reset_pc ? m->reset_pc : lr.entry;
  m->cpu.image_base = lr.image_base;
  lr.isa->init(&m->cpu);
  LogInfo("loaded %s: entry=%llx isa=%s htif=%d", a.image, (unsigned long long)lr.entry,
          lr.isa->name, lr.has_htif);

  BoardRun(m, a.max_inst);

  int code = m->cpu.exit_code;
  LogInfo("stopped: inst=%llu pc=%llx exit=%d", (unsigned long long)m->cpu.inst_count,
          (unsigned long long)m->cpu.pc, code);
  if (code != 0 || a.dump_regs) lr.isa->dump_regs(&m->cpu);

  BoardDestroy(m);
  return code & 0xff;
}
