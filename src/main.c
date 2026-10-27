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
  const char* kernel_path;  // -kernel FILE: payload at board kernel_base
  const char* append;  // -append STR: cmdline into /chosen/bootargs
  const char* hda;        // -hda FILE: primary IDE master image (x86 PC board)
  const char* hdb;        // -hdb FILE: primary IDE slave image
  const char* cdrom;      // -cdrom FILE: secondary IDE master CD-ROM image
  const char* fda;        // -fda FILE: the PC's A: floppy image
  const char* dsk;        // -dsk FILE: a card-slot disk image (oberon board)
  const char* vgabios;    // -vgabios FILE: the display card's option ROM
  const char* display_backend;  // -display win32; NULL = headless
  const char* mouse_name;       // -mouse ps2|serial (NULL = the board's default)
  uint64_t mem_size;
  uint64_t mem_base;
  uint64_t bin_base;
  uint64_t bin_tohost;
  uint64_t max_inst;
  uint32_t switches;  // --switches N: a machine's front-panel switches
  int dump_regs;
  int skip_idle;  // --skip-idle: a halted CPU's idle time runs at full speed
  int icount;         // -icount: the emulated clock is declared, not wall time
  int icount_shift;   // log2 of the nanoseconds one retired instruction costs
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
      "  -icount shift=N   charge 1<<N ns of emulated time to every retired\n"
      "                    instruction, so timer edges are a function of the\n"
      "                    instruction stream and a run reproduces exactly\n"
      "  -bios FILE        x86: map a firmware ROM image and reset into it\n"
      "                    virt: load the firmware at the DRAM base, what\n"
      "                    OpenSBI fw_jump expects (QEMU's -bios)\n"
      "  -kernel FILE      raw payload at the board's kernel address (virt:\n"
      "                    0x80200000, what OpenSBI fw_jump jumps to)\n"
      "  -append STR       kernel command line -> device tree /chosen/bootargs\n"
      "  -hda FILE         x86: primary IDE master disk image\n"
      "  -hdb FILE         x86: primary IDE slave disk image\n"
      "  -cdrom FILE       x86: secondary IDE master CD-ROM image\n"
      "  -fda FILE         x86: floppy image for drive A:\n"
      "  -dsk FILE         disk image for the machine's card slot (oberon:\n"
      "                    the SD card the boot ROM loads the system from)\n"
      "  --switches N      front-panel switches (oberon: bit 0 boots from the\n"
      "                    RS232 channel instead of the card)\n"
      "  -vgabios FILE     x86: the display card's option ROM (int10 services)\n"
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
    else if (strcmp(arg, "-icount") == 0) {
      // QEMU's spelling (-icount shift=N: one retired instruction = 1<<N ns).
      // The bound keeps HostTimerIcountAdvance's int64 ns accumulator in range:
      // shift=30 charges 1.07e9 ns a step, so it holds to 8.6e9 instructions,
      // and anything above 31 would bill over two seconds of virtual time per
      // instruction. A malformed spec is an error, not a silent fallback to the
      // wall clock — that would hand back the run we were asked to make
      // deterministic.
      const char* spec = argv[++i];
      unsigned shift;
      if (sscanf(spec, "shift=%u", &shift) != 1 || shift > 30) {
        LogError("-icount expects shift=N with N in 0..30");
        return -1;
      }
      a->icount = 1;
      a->icount_shift = (int)shift;
    }
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
    else if (strcmp(arg, "-kernel") == 0)
      a->kernel_path = argv[++i];
    else if (strcmp(arg, "-append") == 0)
      a->append = argv[++i];
    else if (strcmp(arg, "-hda") == 0)
      a->hda = argv[++i];
    else if (strcmp(arg, "-hdb") == 0)
      a->hdb = argv[++i];
    else if (strcmp(arg, "-cdrom") == 0)
      a->cdrom = argv[++i];
    else if (strcmp(arg, "-fda") == 0)
      a->fda = argv[++i];
    else if (strcmp(arg, "-dsk") == 0)
      a->dsk = argv[++i];
    else if (strcmp(arg, "--switches") == 0) {
      uint64_t v;
      if (ParseU64(argv[++i], &v) != 0 || v > 0xFFFFFFFFULL) return -1;
      a->switches = (uint32_t)v;
    }
    else if (strcmp(arg, "-vgabios") == 0)
      a->vgabios = argv[++i];
    else if (strcmp(arg, "-mouse") == 0)
      a->mouse_name = argv[++i];
    else if (arg[0] == '-' && arg[1] == '-')
      return -1;
    else
      a->image = arg;
  }
  // An image file is the normal way in; -bios boots the machine from its
  // firmware ROM instead, and -dsk hands a card-slot machine its disk — either
  // way main() rejects boards that cannot run without a loaded image.
  if (!a->image && !a->bios_path && !a->dsk) return -1;
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
    return kExitHostFailure;
  }
  if (a.log_file) LogInitFile(a.log_file);

  // -icount goes in before the machine exists: the emulated clock must not
  // advance while the machine is built or an image is loaded, or the time the
  // guest sees would again depend on how long that took.
  if (a.icount) {
    HostTimerIcountEnable(a.icount_shift);
    LogInfo("-icount shift=%d: one retired instruction = %llu ns of emulated time",
            a.icount_shift, (unsigned long long)1 << a.icount_shift);
  }

  // -mouse: which pointing device the host's pointer drives (the PC board is
  // the only one with a choice). An unknown name is an error, not a silent
  // fallback to the default.
  int mouse = kMousePs2;
  if (a.mouse_name) {
    if (strcmp(a.mouse_name, "ps2") == 0)
      mouse = kMousePs2;
    else if (strcmp(a.mouse_name, "serial") == 0)
      mouse = kMouseSerial;
    else {
      LogError("-mouse expects ps2 or serial");
      return kExitHostFailure;
    }
  }

  BoardOpts opts = {.ram_base = a.mem_base,
                    .ram_size = a.mem_size,
                    .bios_path = a.bios_path,
                    .kernel_path = a.kernel_path,
                    .append = a.append,
                    .hda = a.hda,
                    .hdb = a.hdb,
                    .cdrom = a.cdrom,
                    .fda = a.fda,
                    .dsk = a.dsk,
                    .switches = a.switches,
                    .vgabios_path = a.vgabios,
                    .skip_idle = a.skip_idle,
                    .mouse = mouse};
  Board* m = BoardCreate(a.machine_name, &opts);
  if (!m) return kExitHostFailure;

  // -kernel: raw payload into the board's kernel slot (the firmware's
  // follow-on image, QEMU's -kernel). Pure byte delivery -- no entry, no
  // ISA, the firmware owns what happens to it.
  if (a.kernel_path) {
    if (!m->kernel_base) {
      LogError("machine %s has no kernel slot", a.machine_name);
      BoardDestroy(m);
      return kExitHostFailure;
    }
    HostFile* kf = HostFileOpenRead(a.kernel_path);
    if (!kf) {
      LogError("cannot open kernel '%s'", a.kernel_path);
      BoardDestroy(m);
      return kExitHostFailure;
    }
    static char kbuf[1 << 16];
    uint64_t off = 0;
    size_t n;
    while ((n = HostFileRead(kf, kbuf, sizeof kbuf)) > 0) {
      uint8_t* dst;
      if (BusRamRange(&m->bus, m->kernel_base + off, n, &dst) != 0) {
        LogError("kernel image runs past RAM at +0x%llx", (unsigned long long)off);
        HostFileClose(kf);
        BoardDestroy(m);
        return kExitHostFailure;
      }
      memcpy(dst, kbuf, n);
      off += n;
    }
    HostFileClose(kf);
    LogInfo("kernel %s: 0x%llx bytes at 0x%llx", a.kernel_path,
            (unsigned long long)off, (unsigned long long)m->kernel_base);
  }

  uint64_t bin_base = a.bin_base ? a.bin_base : m->bin_base;
  LoadResult lr;
  memset(&lr, 0, sizeof(lr));
  // On boards whose -bios is a plain image (virt firmware), it takes the
  // image slot; on the PC board the board itself mapped the ROM during
  // creation, so m->bios_image stays 0 and a positional image is separate.
  const char* image_path = a.image ? a.image : (m->bios_image ? a.bios_path : NULL);
  if (image_path) {
    if (LoaderLoadImage(&m->bus, image_path, a.isa_name, bin_base, a.bin_tohost, m->bin_htif,
                        &lr) != 0) {
      BoardDestroy(m);
      return kExitHostFailure;
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
      return kExitHostFailure;
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
  if (image_path)
    LogInfo("loaded %s: entry=%llx isa=%s htif=%d", image_path, (unsigned long long)lr.entry,
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
      return kExitHostFailure;
    }
    if (strcmp(a.display_backend, "win32") != 0) {
      LogError("unknown display backend %s (win32)", a.display_backend);
      BoardDestroy(m);
      return kExitHostFailure;
    }
    char title[128];
    snprintf(title, sizeof(title), "cemu %s - %s", a.machine_name, a.image ? a.image : "firmware");
    m->display = HostDisplayOpen(title, m->display_ops, m->display_dev);
    if (!m->display) {
      BoardDestroy(m);
      return kExitHostFailure;
    }
    // The board's keyboard sink (the PC's 8042): window keys drive it, and
    // the pointer sinks drive the same controller's auxiliary port. The
    // absolute sink is the window's default when the board has one; the
    // relative sink remains for the debug hub's synthetic events.
    if (m->key_in) HostDisplaySetKeySink(m->display, m->key_in, m->key_ctx);
    if (m->mouse_in) HostDisplaySetMouseSink(m->display, m->mouse_in, m->mouse_ctx);
    if (m->mouse_abs) HostDisplaySetMouseAbsSink(m->display, m->mouse_abs, m->mouse_abs_ctx);
  }

  if (a.gdb_port) {
    m->gdb = GdbStubStart(m, a.gdb_port, a.gdb_wait);
    if (!m->gdb) {
      BoardDestroy(m);
      return kExitHostFailure;
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
