#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board/board.h"
#include "board/loader.h"
#include "debug/gdbstub.h"
#include "host/host.h"
#include "util/log.h"

typedef struct Args {
  const char* image;
  const char* isa_name;
  const char* machine_name;
  const char* log_file;
  const char* bios_path;  // -bios FILE: firmware ROM image (x86 PC board)
  const char* hda;        // -hda FILE: primary IDE master image (x86 PC board)
  const char* hdb;        // -hdb FILE: primary IDE slave image
  const char* cdrom;      // -cdrom FILE: secondary IDE master CD-ROM image
  const char* display_backend;  // -display win32; NULL = headless
  uint64_t mem_size;
  uint64_t mem_base;
  uint64_t bin_base;
  uint64_t bin_tohost;
  uint64_t max_inst;
  int dump_regs;
  int skip_idle;  // --skip-idle: a halted CPU's idle time runs at full speed
  int gdb_port;  // -s / -gdb tcp::PORT; 0 = no stub
  int gdb_wait;  // -S: stopped until a client resumes (QEMU convention)
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
      "  --dump-regs       dump registers on any exit\n"
      "  --skip-idle       jump a halted CPU to the next device deadline\n"
      "                    instead of letting the wait elapse in host time\n"
      "  -bios FILE        x86: map a firmware ROM image and reset into it\n"
      "  -hda FILE         x86: primary IDE master disk image\n"
      "  -hdb FILE         x86: primary IDE slave disk image\n"
      "  -cdrom FILE       x86: secondary IDE master CD-ROM image\n"
      "  -display win32    open a window on the machine's display card\n"
      "  -s                gdb stub on tcp::1234 (guest runs until attached)\n"
      "  -gdb tcp::PORT    gdb stub on PORT\n"
      "  -S                with -s: do not start until the client resumes\n";
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
    else if (strcmp(arg, "--skip-idle") == 0)
      a->skip_idle = 1;
    else if (strcmp(arg, "-display") == 0) {
      if (i + 1 >= argc) {
        LogError("-display expects a backend name");
        return -1;
      }
      a->display_backend = argv[++i];
    } else if (strcmp(arg, "-s") == 0)
      a->gdb_port = 1234;  // the QEMU -s convention
    else if (strcmp(arg, "-gdb") == 0) {
      // QEMU: -gdb tcp::PORT (the host part is empty = all interfaces).
      const char* spec = argv[++i];
      const char* p = strncmp(spec, "tcp::", 5) == 0 ? spec + 5 : NULL;
      if (!p || !*p) {
        LogError("-gdb expects tcp::PORT");
        return -1;
      }
      a->gdb_port = (int)strtoul(p, NULL, 10);
      if (a->gdb_port <= 0 || a->gdb_port > 65535) {
        LogError("-gdb: bad port");
        return -1;
      }
    } else if (strcmp(arg, "-S") == 0)
      a->gdb_wait = 1;
    else if (strcmp(arg, "-bios") == 0)
      a->bios_path = argv[++i];
    else if (strcmp(arg, "-hda") == 0)
      a->hda = argv[++i];
    else if (strcmp(arg, "-hdb") == 0)
      a->hdb = argv[++i];
    else if (strcmp(arg, "-cdrom") == 0)
      a->cdrom = argv[++i];
    else if (arg[0] == '-' && arg[1] == '-')
      return -1;
    else
      a->image = arg;
  }
  // An image file is the normal way in; -bios boots the machine from its
  // firmware ROM instead, and main() rejects boards that cannot do that.
  if (!a->image && !a->bios_path) return -1;
  if (a->gdb_wait && !a->gdb_port) {
    LogError("-S needs -s or -gdb");
    return -1;
  }
  return 0;
}

int main(int argc, char** argv) {
  Args a;
  if (ParseArgs(&a, argc, argv) != 0) {
    Usage();
    return 1;
  }
  if (a.log_file) LogInitFile(a.log_file);

  BoardOpts opts = {.ram_base = a.mem_base,
                    .ram_size = a.mem_size,
                    .bios_path = a.bios_path,
                    .hda = a.hda,
                    .hdb = a.hdb,
                    .cdrom = a.cdrom,
                    .skip_idle = a.skip_idle};
  Board* m = BoardCreate(a.machine_name, &opts);
  if (!m) return 1;

  uint64_t bin_base = a.bin_base ? a.bin_base : m->bin_base;
  LoadResult lr;
  memset(&lr, 0, sizeof(lr));
  if (a.image) {
    if (LoaderLoadImage(&m->bus, a.image, a.isa_name, bin_base, a.bin_tohost, m->bin_htif, &lr) !=
        0) {
      BoardDestroy(m);
      return 1;
    }
  } else {
    // Firmware boot (-bios): nothing is loaded, so the machine names its own
    // CPU model and the board brings its own reset state (the PC's reset
    // vector inside the BIOS ROM window). Boards without a default ISA need
    // an image file, which then names the ISA itself.
    const char* isa_name = a.isa_name ? a.isa_name : m->default_isa;
    lr.isa = isa_name ? LoaderFindIsa(isa_name) : NULL;
    if (!lr.isa) {
      LogError("machine %s has no default isa: give it an image file", a.machine_name);
      BoardDestroy(m);
      return 1;
    }
    lr.entry = m->reset_pc;
  }
  if (lr.has_htif) {
    HtifBind(&m->htif, &m->cpu);
    HtifRegister(&m->bus, &m->htif, lr.tohost, lr.fromhost);
  }
  m->isa = lr.isa;
  m->cpu.pc = m->reset_pc ? m->reset_pc : lr.entry;
  m->entry = m->cpu.pc;  // where a machine reset restarts (board.h)
  m->cpu.image_base = lr.image_base;
  lr.isa->init(&m->cpu);
  if (a.image)
    LogInfo("loaded %s: entry=%llx isa=%s htif=%d", a.image, (unsigned long long)lr.entry,
            lr.isa->name, lr.has_htif);
  else
    LogInfo("firmware boot: reset=%llx isa=%s", (unsigned long long)m->cpu.pc, lr.isa->name);

  // Host input: stdin feeds the machine's sinks — its serial receiver when it
  // has one (raw bytes: a terminal on COM1, or the virt machine's ns16550a),
  // otherwise its keyboard (set-1 scan codes). This is deliberately outside the
  // display branch: a run without a window has stdin as its only input path,
  // and the window path is attached separately below.
  if (m->serial_in || m->key_in) {
    HostInputOpen(m->serial_in, m->serial_ctx, m->key_in, m->key_ctx);
  }

  if (a.display_backend) {
    if (!m->display_ops) {
      LogError("machine %s has no display card", a.machine_name);
      BoardDestroy(m);
      return 1;
    }
    if (strcmp(a.display_backend, "win32") != 0) {
      LogError("unknown display backend %s (win32)", a.display_backend);
      BoardDestroy(m);
      return 1;
    }
    char title[128];
    snprintf(title, sizeof(title), "cemu %s - %s", a.machine_name, a.image ? a.image : "firmware");
    m->display =
        HostDisplayOpen(title, m->display_ops->width, m->display_ops->height,
                        m->display_ops->Framebuffer(m->display_dev),
                        m->display_ops->Version, m->display_dev);
    if (!m->display) {
      BoardDestroy(m);
      return 1;
    }
    // The board's keyboard sink (the PC's 8042): window keys drive it.
    if (m->key_in) HostDisplaySetKeySink(m->display, m->key_in, m->key_ctx);
  }

  if (a.gdb_port) {
    m->gdb = GdbStubStart(m, a.gdb_port, a.gdb_wait);
    if (!m->gdb) {
      BoardDestroy(m);
      return 1;
    }
  }

  BoardRun(m, a.max_inst);

  int code = m->cpu.exit_code;
  LogInfo("stopped: inst=%llu pc=%llx exit=%d", (unsigned long long)m->cpu.inst_count,
          (unsigned long long)m->cpu.pc, code);
  if (code != 0 || a.dump_regs) lr.isa->dump_regs(&m->cpu);

  GdbStubFree(m->gdb);
  HostDisplayFree(m->display);
  BoardDestroy(m);
  return code & 0xff;
}
