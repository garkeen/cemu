// The debug hub: CEMU_DEBUG parsing, the event table, watchpoints, budget,
// and the session summary. Everything renders through util/table.c and
// HostWriteErr (streaming, ordered). Register naming goes through the ISA's
// DebugLayout so the core stays ISA-blind (AGENTS.md §X).
#include "debug/debug.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus/bus.h"
#include "cpu/step.h"
#include "host/host.h"
#include "util/log.h"
#include "util/table.h"

static uint32_t g_mask;
static int g_inited;
static int g_ascii = 1;  // plain borders by default (console codepages
                         // mangle UTF-8); `utf8` switches on
static int g_header_every = 40;
static int g_regs_period;      // 0 = off
static int g_budget = 100000;  // per-category event cap
static uint64_t g_skip = 0;    // per-category events suppressed BEFORE printing

// Hard session output cap (bytes). Every debug write is accounted here; once
// the cap is hit, further writes are dropped so a misconfigured CEMU_DEBUG
// can't fill the disk (the `state` category alone can otherwise emit gigabytes
// since it bypasses the per-category budget). ~5MB is plenty for any real
// diagnosis; raise it with a code change if you genuinely need more.
static const uint64_t kOutLimit = 5ULL * 1024 * 1024;
static uint64_t g_out_bytes;
static int g_limit_hit;

// The one event table every category shares (AGENTS.md §X).
static const TableColumn kEventCols[] = {
    {"KIND", 4, kTableLeft},      {"PC", 16, kTableRight},    {"RAW", 12, kTableLeft},
    {"MNEMONIC", 19, kTableLeft}, {"DETAIL", 41, kTableLeft}, {"FLAGS", 6, kTableLeft},
};
static Table* g_tbl;

// Per-category counters for the budget suppression and the summary.
static uint64_t g_emitted[8];
static uint64_t g_suppressed[8];
static uint64_t g_traps;

// ---- watchpoints ----------------------------------------------------------

enum { kMaxWatches = 16 };
static struct {
  uint64_t addr;
  int size;
  int rw;  // 1 read, 2 write, 3 both
} g_watch[kMaxWatches];
static int g_nwatch;

int DebugWatchHit(uint64_t addr, int size, int is_load) {
  int want = is_load ? 1 : 2;
  for (int i = 0; i < g_nwatch; i++)
    if (g_watch[i].rw & want && addr < g_watch[i].addr + (uint64_t)g_watch[i].size &&
        addr + (uint64_t)size > g_watch[i].addr)
      return 1;
  return 0;
}

// ---- guest-memory dumps -----------------------------------------------------
//
// watch= reports that an address was touched; it cannot report what a guest
// left in a structure nothing reads again (a stopped kernel's log buffer, its
// page tables, its task structs). Those need the address space, which this hub
// does not own — bus/ stays outside debug/ — so the board installs a reader
// (DebugSetMemReader) and dump items are written when the session ends.
enum { kMaxDumps = 4 };
static int ParseUint(const char* s, uint64_t* out);  // defined with the watch parser below
static struct {
  uint64_t addr;
  uint64_t size;
  char file[128];
} g_dump[kMaxDumps];
static int g_ndump;
static DebugMemReadFn g_mem_read;
static void* g_mem_ctx;
// AGENTS.md §七 caps every file this repo writes at 5MB; a dump is a file this
// repo writes, so an oversized region is refused rather than truncated.
static const uint64_t kDumpLimit = 5ULL * 1024 * 1024;

void DebugSetMemReader(DebugMemReadFn read, void* ctx) {
  g_mem_read = read;
  g_mem_ctx = ctx;
}

// dump=ADDR:SIZE:FILE — the address is physical, exactly as in watch=.
static void AddDump(const char* spec) {
  if (g_ndump >= kMaxDumps) {
    LogError("debug: too many dumps, dropping '%s'", spec);
    return;
  }
  const char* c1 = strchr(spec, ':');
  if (!c1) {
    LogError("debug: dump expects ADDR:SIZE:FILE ('%s')", spec);
    return;
  }
  const char* c2 = strchr(c1 + 1, ':');
  if (!c2) {
    LogError("debug: dump expects ADDR:SIZE:FILE, and the file is not optional ('%s')", spec);
    return;
  }
  char head[32], szs[32];
  size_t hl = (size_t)(c1 - spec), sl = (size_t)(c2 - c1 - 1);
  if (hl >= sizeof(head) || sl >= sizeof(szs) || strlen(c2 + 1) >= sizeof(g_dump[0].file)) {
    LogError("debug: dump field too long ('%s')", spec);
    return;
  }
  memcpy(head, spec, hl);
  head[hl] = 0;
  memcpy(szs, c1 + 1, sl);
  szs[sl] = 0;
  uint64_t addr, size;
  if (!ParseUint(head, &addr) || !ParseUint(szs, &size) || size == 0) {
    LogError("debug: dump address/size must be numbers, 0x-prefixed for hex ('%s')", spec);
    return;
  }
  g_dump[g_ndump].addr = addr;
  g_dump[g_ndump].size = size;
  snprintf(g_dump[g_ndump].file, sizeof(g_dump[0].file), "%s", c2 + 1);
  g_ndump++;
}

// Writes one region out at session end. A short read is a failed dump, not a
// partial file: the guest's memory is the whole point, so a region that does
// not resolve is reported and skipped.
static void WriteDump(const int idx) {
  const uint64_t addr = g_dump[idx].addr, size = g_dump[idx].size;
  const char* path = g_dump[idx].file;
  char line[192];
  if (!g_mem_read) {
    LogError("debug: dump %s: no memory reader (the board installed none)", path);
    return;
  }
  if (size > kDumpLimit) {
    LogError("debug: dump %s: %llu bytes exceeds the 5MB session file cap", path,
             (unsigned long long)size);
    return;
  }
  uint8_t* buf = (uint8_t*)malloc((size_t)size);
  if (!buf) {
    LogError("debug: dump %s: out of memory", path);
    return;
  }
  if (!g_mem_read(g_mem_ctx, addr, buf, (int)size)) {
    LogError("debug: dump %s: no memory at %llx for %llu bytes", path, (unsigned long long)addr,
             (unsigned long long)size);
    free(buf);
    return;
  }
  HostFile* f = HostFileCreate(path);
  if (!f) {
    LogError("debug: dump %s: cannot create the file", path);
    free(buf);
    return;
  }
  size_t wrote = HostFileWriteAt(f, 0, buf, (size_t)size);
  HostFileClose(f);
  free(buf);
  int n = snprintf(line, sizeof(line), "== dump: %llx:%llu -> %s (%llu bytes)\n",
                   (unsigned long long)addr, (unsigned long long)size, path,
                   (unsigned long long)wrote);
  if (n > 0 && DebugAccountOut((size_t)n)) HostWriteErr(line, (size_t)n);
}

// ---- init -------------------------------------------------------------------

static int ParseUint(const char* s, uint64_t* out) {
  char* end;
  *out = strtoull(s, &end, 0);
  return *s && *end == 0;
}

static void AddWatch(const char* spec) {
  // watch=ADDR:SIZE[:r|w|rw]
  if (g_nwatch >= kMaxWatches) {
    LogError("debug: too many watches, dropping '%s'", spec);
    return;
  }
  uint64_t addr, size;
  const char* colon = strchr(spec, ':');
  if (!colon) {
    LogError("debug: watch expects ADDR:SIZE[:r|w|rw] ('%s')", spec);
    return;
  }
  char head[32];
  size_t hl = (size_t)(colon - spec);
  if (hl >= sizeof(head)) return;
  memcpy(head, spec, hl);
  head[hl] = 0;
  if (!ParseUint(head, &addr)) {
    LogError("debug: watch address must be a number, 0x-prefixed for hex ('%s')", head);
    return;
  }
  const char* rest = colon + 1;
  const char* colon2 = strchr(rest, ':');
  char szs[32];
  size_t sl = colon2 ? (size_t)(colon2 - rest) : strlen(rest);
  if (sl >= sizeof(szs)) return;
  memcpy(szs, rest, sl);
  szs[sl] = 0;
  if (!ParseUint(szs, &size)) {
    LogError("debug: watch size must be a number ('%s')", szs);
    return;
  }
  int rw = 3;
  if (colon2) {
    if (!strcmp(colon2 + 1, "r"))
      rw = 1;
    else if (!strcmp(colon2 + 1, "w"))
      rw = 2;
  }
  g_watch[g_nwatch].addr = addr;
  g_watch[g_nwatch].size = (int)size;
  g_watch[g_nwatch].rw = rw;
  g_nwatch++;
}

// ---- synthetic host input (mouse=) -----------------------------------------

enum { kMaxMouseInj = 4 };
static struct {
  int dx, dy, dz, buttons;
  uint64_t period;   // instructions between events
  uint64_t next_at;  // the instruction count of the next one
} g_mouse_inj[kMaxMouseInj];
static int g_nmouse_inj;

int DebugNextMouseEvent(uint64_t inst_count, DebugInjection* out) {
  for (int i = 0; i < g_nmouse_inj; i++) {
    if (inst_count < g_mouse_inj[i].next_at) continue;
    // Catch up in one step if the item came due while nobody was looking, so a
    // backlog does not arrive as a burst.
    do {
      g_mouse_inj[i].next_at += g_mouse_inj[i].period;
    } while (g_mouse_inj[i].next_at <= inst_count);
    out->dx = g_mouse_inj[i].dx;
    out->dy = g_mouse_inj[i].dy;
    out->dz = g_mouse_inj[i].dz;
    out->buttons = g_mouse_inj[i].buttons;
    if (DebugOn(kDbgMark)) DebugMark("mouse-inj", out->dx, out->dy);
    return 1;
  }
  return 0;
}

// mouse=DX:DY:BUTTONS[:WHEEL]@N — signed C literals (hex needs 0x), and the
// period is required: a default would silently pick a cadence for the caller.
static void AddMouseInjection(const char* spec) {
  if (g_nmouse_inj >= kMaxMouseInj) {
    LogError("debug: too many mouse= items, dropping '%s'", spec);
    return;
  }
  char body[128];
  if (strlen(spec) >= sizeof(body)) {
    LogError("debug: mouse= item too long ('%s')", spec);
    return;
  }
  strcpy(body, spec);
  char* at = strrchr(body, '@');
  if (!at) {
    LogError("debug: mouse= expects DX:DY:BUTTONS[:WHEEL]@N ('%s')", spec);
    return;
  }
  *at = 0;
  long vals[4] = {0, 0, 0, 0};
  int n = 0;
  char* save = NULL;
  for (char* f = strtok_r(body, ":", &save); f && n < 4; f = strtok_r(NULL, ":", &save)) {
    char* end = NULL;
    vals[n] = strtol(f, &end, 0);
    if (!*f || !end || *end) {
      LogError("debug: mouse= field '%s' is not a number ('%s')", f, spec);
      return;
    }
    n++;
  }
  if (n < 3) {
    LogError("debug: mouse= needs at least DX:DY:BUTTONS ('%s')", spec);
    return;
  }
  char* end = NULL;
  long period = strtol(at + 1, &end, 0);
  if (!end || *end || period <= 0) {
    LogError("debug: mouse= period must be a positive number ('%s')", spec);
    return;
  }
  g_mouse_inj[g_nmouse_inj].dx = (int)vals[0];
  g_mouse_inj[g_nmouse_inj].dy = (int)vals[1];
  g_mouse_inj[g_nmouse_inj].buttons = (int)vals[2];
  g_mouse_inj[g_nmouse_inj].dz = (int)vals[3];
  g_mouse_inj[g_nmouse_inj].period = (uint64_t)period;
  g_mouse_inj[g_nmouse_inj].next_at = (uint64_t)period;
  g_nmouse_inj++;
}

void DebugInit(void) {
  g_inited = 1;
  const char* spec = getenv("CEMU_DEBUG");
  if (!spec) return;

  char buf[512];
  snprintf(buf, sizeof(buf), "%s", spec);
  char* save = NULL;
  for (char* tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
    if (!strcmp(tok, "trace") || !strcmp(tok, "trace:line"))
      g_mask |= kDbgTraceLine;
    else if (!strcmp(tok, "trace:table"))
      g_mask |= kDbgTraceTable;
    else if (!strcmp(tok, "state"))
      g_mask |= kDbgState;
    else if (!strcmp(tok, "mem") || !strcmp(tok, "mem:ld") || !strcmp(tok, "mem:st"))
      g_mask |= kDbgMem;
    else if (!strcmp(tok, "trap"))
      g_mask |= kDbgTrap;
    else if (!strcmp(tok, "bus"))
      g_mask |= kDbgBus;
    else if (!strcmp(tok, "gdb"))
      g_mask |= kDbgGdb;
    else if (!strcmp(tok, "mark"))
      g_mask |= kDbgMark;
    else if (!strcmp(tok, "screen"))
      g_mask |= kDbgScreen;
    else if (!strncmp(tok, "regs=", 5))
      g_regs_period = atoi(tok + 5);
    else if (!strncmp(tok, "watch=", 6))
      AddWatch(tok + 6);
    else if (!strncmp(tok, "dump=", 5))
      AddDump(tok + 5);
    else if (!strncmp(tok, "mouse=", 6))
      AddMouseInjection(tok + 6);
    else if (!strncmp(tok, "budget=", 7))
      g_budget = atoi(tok + 7);
    else if (!strncmp(tok, "skip=", 5))
      g_skip = strtoull(tok + 5, NULL, 0);
    else if (!strcmp(tok, "utf8"))
      g_ascii = 0;
    else
      LogError("debug: unknown CEMU_DEBUG item '%s'", tok);
  }
  if (g_mask &
          (kDbgTraceTable | kDbgMem | kDbgTrap | kDbgBus | kDbgGdb | kDbgMark | kDbgScreen) ||
      g_nwatch) {
    g_tbl = TableOpen(kEventCols, 6, g_ascii, g_header_every);
    TableHeader(g_tbl);
  }
}

int DebugOn(uint32_t cat) { return g_inited && ((g_mask & cat) || (g_nwatch && cat == kDbgMem)); }

// ---- rendering helpers ------------------------------------------------------

// Category index for the budget counters (bit position clamped into range).
static int CatIdx(uint32_t cat) {
  for (int i = 0; i < 8; i++)
    if (cat & (1u << i)) return i;
  return 7;
}

// The window is closed; the budget counts from here.
static int AllowAfterWindow(uint32_t cat) {
  int i = CatIdx(cat);
  if (g_emitted[i] - g_skip < (uint64_t)g_budget) {
    g_emitted[i]++;
    return 1;
  }
  g_suppressed[i]++;
  return 0;
}

// Budget gate: 1 = emit, 0 = suppress (counted for the summary). The skip
// window here is counted in this category's own events.
static int Allow(uint32_t cat) {
  int i = CatIdx(cat);
  if (g_emitted[i] < g_skip) {  // skip window: count, don't print
    g_emitted[i]++;
    return 0;
  }
  return AllowAfterWindow(cat);
}

// Trace gate. A trace `skip=N` reads as "say nothing before instruction N",
// and its unit is instructions rather than events: the trace stream has no
// event for a step that retires nothing (a REP iteration, a hlt wakeup), so an
// event counter closes the window at an instruction number nobody asked for —
// and when N sits near the run's length it never closes at all, which looks
// like a silent trace (D29). The retired-instruction counter is the unit the
// caller means.
static int AllowTrace(uint32_t cat, uint64_t inst) {
  if (inst < g_skip) return 0;
  int i = CatIdx(cat);
  if (g_emitted[i] < g_skip) g_emitted[i] = g_skip;  // the budget starts at the window's edge
  return AllowAfterWindow(cat);
}

// Session output cap. Returns 1 if n bytes may still be written (and accounts
// for them); 0 once the cap is hit. table.c's Flush routes through here.
int DebugAccountOut(size_t n) {
  if (g_limit_hit) return 0;
  if (g_out_bytes >= kOutLimit) {
    g_limit_hit = 1;
    static const char msg[] =
        "\n[CEMU_DEBUG: output limit 5MB reached — further output dropped]\n";
    HostWriteErr(msg, sizeof(msg) - 1);
    return 0;
  }
  if (g_out_bytes + (uint64_t)n > kOutLimit) {
    g_limit_hit = 1;
    static const char msg[] =
        "\n[CEMU_DEBUG: output limit 5MB reached — further output dropped]\n";
    HostWriteErr(msg, sizeof(msg) - 1);
    return 0;
  }
  g_out_bytes += (uint64_t)n;
  return 1;
}

static void CellRaw(Table* t, const frame* f) {
  char c[32];
  int n = 0;
  for (int i = 0; i < f->rec.raw_len && i < 8; i++)
    n += snprintf(c + n, sizeof(c) - (size_t)n, "%02x ", f->rec.raw[i]);
  if (n) c[n - 1] = 0;  // trailing blank
  TableRowCell(t, c);
}

static void CellFlags(Table* t, frame* f) {
  if (f->isa->flag_names && f->isa->flag_word) {
    char c[32];
    uint64_t v = f->isa->flag_word(f);
    int n = 0;
    for (int i = 0; f->isa->flag_names[i] && n < (int)sizeof(c) - 6; i++)
      if (v & (1ULL << i))
        n += snprintf(c + n, sizeof(c) - (size_t)n, "%s ", f->isa->flag_names[i]);
    if (n) c[n - 1] = 0;
    TableRowCell(t, n ? c : "-");
    return;
  }
  TableRowCell(t, "-");
}

// Rows emitted while fetch is still filling insn (the L lines of the fetch
// itself) must not read insn fields — pass NULL and blank them.
static void RowBeginBare(Table* t, char kind) {
  TableRowBegin(t);
  char k[2] = {kind, 0};
  TableRowCell(t, k);
  TableRowCell(t, "");
  TableRowCell(t, "");
  TableRowCell(t, "");
}

static void RowBegin(Table* t, char kind, frame* f) {
  TableRowBegin(t);
  char k[2] = {kind, 0};
  TableRowCell(t, k);
  char pc[20];
  snprintf(pc, sizeof(pc), "%016llx", (unsigned long long)f->rec.pc);
  TableRowCell(t, pc);
  CellRaw(t, f);
  TableRowCell(t, f->rec.mnemonic ? f->rec.mnemonic : "");
}

// ---- state stream -----------------------------------------------------------

// One canonical line per committed instruction: register banks in cell
// order (never mnemonic order) so the format survives refactors and is the
// diff baseline for golden runs (AGENTS.md §X).
static void StateRow(frame* f) {
  char line[1024];
  size_t cap = sizeof(line);
  int n = snprintf(line, (size_t)cap, "S @pc=%016llx dnpc=%016llx | ",
                   (unsigned long long)f->rec.pc, (unsigned long long)f->rec.dnpc);
  for (int bank = 0; bank < 2; bank++) {
    if (bank == 1 && !(f->isa->has_fpr && f->isa->has_fpr(f)))
      break;  // x86 has no FP bank in the stream
    for (int c = 0; c < 32 && n < (int)cap - 24; c++) {
      uint64_t v = bank == 0 ? f->cpu->gpr[c] : f->cpu->fpr[c];
      n += snprintf(line + n, cap - (size_t)n, "%c%d=%016llx ", bank == 0 ? 'g' : 'f', c,
                    (unsigned long long)v);
    }
    n += snprintf(line + n, cap - (size_t)n, "| ");
  }
  if (f->isa->debug_state_line)
    f->isa->debug_state_line(line + n, (int)(cap - (size_t)n), f);
  size_t len = strlen(line);
  if (DebugAccountOut(len + 1)) {
    HostWriteErr(line, len);
    HostWriteErr("\n", 1);
  }
}

// ---- emission points --------------------------------------------------------

void DebugInsn(frame* f) {
  if (!g_inited) return;
  if (g_mask & kDbgTraceLine) {
    // Compact one-line form: pc, raw bytes, mnemonic, next pc. Greppable,
    // no table chrome. The skip window is in instructions (AllowTrace).
    if (AllowTrace(kDbgTraceLine, f->cpu->inst_count)) {
      char line[160];
      int n = snprintf(line, sizeof(line), "pc=%016llx", (unsigned long long)f->rec.pc);
      for (int i = 0; i < f->rec.raw_len && i < 8; i++)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %02x", f->rec.raw[i]);
      n += snprintf(line + n, sizeof(line) - (size_t)n, " %s -> %016llx\n",
                    f->rec.mnemonic ? f->rec.mnemonic : "?", (unsigned long long)f->rec.dnpc);
      if (DebugAccountOut((size_t)n)) HostWriteErr(line, (size_t)n);
    }
  }
  if (g_mask & kDbgTraceTable) {
    if (AllowTrace(kDbgTraceTable, f->cpu->inst_count)) {
      RowBegin(g_tbl, 'I', f);
      char det[48];
      snprintf(det, sizeof(det), "-> %016llx", (unsigned long long)f->rec.dnpc);
      TableRowCell(g_tbl, det);
      CellFlags(g_tbl, f);
      TableRowEnd(g_tbl);
    }
  }
  if (g_mask & kDbgState) StateRow(f);
  if (g_regs_period && (f->cpu->inst_count % (uint64_t)g_regs_period) == 0) {
    // Periodic register table reuses DumpRegs through a Frame-less path.
    f->isa->dump_regs(f->cpu);
  }
}

void DebugTrap(frame* f) {
  g_traps++;
  if (!DebugOn(kDbgTrap) || !Allow(kDbgTrap)) return;
  RowBegin(g_tbl, 'T', f);
  char det[96];
  const char* cn = f->isa->cause_name ? f->isa->cause_name(f->trap.cause) : NULL;
  snprintf(det, sizeof(det), "%s=%llu tval=%016llx -> %016llx", cn ? cn : "cause",
           (unsigned long long)f->trap.cause, (unsigned long long)f->trap.tval,
           (unsigned long long)f->rec.dnpc);
  TableRowCell(g_tbl, det);
  CellFlags(g_tbl, f);
  TableRowEnd(g_tbl);
}

void DebugMem(frame* f, uint64_t addr, int size, int acc, uint64_t val_or_result, int is_load) {
  // Fetch rows: the instruction is still being assembled, so PC/RAW/
  // MNEMONIC are mid-fill. Fetches are their own access class.
  int mid_fetch = acc == 0;  // acc_ifetch: rec still mid-fill
  if (g_mask & kDbgMem) {
    if (Allow(kDbgMem)) {
      if (mid_fetch)
        RowBeginBare(g_tbl, is_load ? 'L' : 'S');
      else
        RowBegin(g_tbl, is_load ? 'L' : 'S', f);
      char det[64];
      snprintf(det, sizeof(det), "%s %llx:%d %s %llx", is_load ? "ld" : "st",
               (unsigned long long)addr, size, is_load ? "->" : "=",
               (unsigned long long)(val_or_result & 0xffffffffULL));
      TableRowCell(g_tbl, det);
      CellFlags(g_tbl, f);
      TableRowEnd(g_tbl);
    }
  }
  if (DebugWatchHit(addr, size, is_load)) {
    if (mid_fetch)
      RowBeginBare(g_tbl, 'W');
    else
      RowBegin(g_tbl, 'W', f);
    char det[64];
    snprintf(det, sizeof(det), "%c %llx:%d %s %llx", is_load ? 'r' : 'w', (unsigned long long)addr,
             size, is_load ? "->" : "<-", (unsigned long long)(val_or_result & 0xffffffffULL));
    TableRowCell(g_tbl, det);
    CellFlags(g_tbl, f);
    TableRowEnd(g_tbl);
  }
}

void DebugBus(frame* f, const char* dev_name, uint64_t addr, int size, int is_load, uint64_t val) {
  if (!DebugOn(kDbgBus) || !Allow(kDbgBus)) return;
  RowBegin(g_tbl, 'B', f);
  char det[64];
  snprintf(det, sizeof(det), "%s %s:%llx:%d %s %llx", is_load ? "io-ld" : "io-st",
           dev_name ? dev_name : "?", (unsigned long long)addr, size, is_load ? "->" : "=",
           (unsigned long long)(val & 0xffffffffULL));
  TableRowCell(g_tbl, det);
  CellFlags(g_tbl, f);
  TableRowEnd(g_tbl);
}

// gdb stub protocol packets (stage 3.5): tx/rx rows, no frame — the stub is
// outside the guest's step stream. Payload is truncated to one line.
void DebugGdbPkt(int is_tx, const char* pkt) {
  if (!DebugOn(kDbgGdb) || !Allow(kDbgGdb)) return;
  RowBeginBare(g_tbl, 'G');
  char det[80];
  snprintf(det, sizeof(det), "%s %.60s", is_tx ? "tx" : "rx", pkt);
  TableRowCell(g_tbl, det);
  TableRowEnd(g_tbl);
}

void DebugGdbNote(const char* note) {
  if (!DebugOn(kDbgGdb) || !Allow(kDbgGdb)) return;
  RowBeginBare(g_tbl, 'G');
  char det[80];
  snprintf(det, sizeof(det), "~~ %.60s", note);
  TableRowCell(g_tbl, det);
  TableRowEnd(g_tbl);
}

// Frameless note rows: the board's interrupt lines and the host input path live
// outside the guest's step stream, and that gap is what made the keyboard path
// invisible (AGENTS.md §IX.1: fix the facility instead of guessing).
// `a` prints decimal (vectors, line numbers, IRQ ids) and `b` hex (bitmasks,
// addresses, controller state) — the two forms a mark's two numbers actually
// need, since one cell cannot hold both.
void DebugMark(const char* what, int a, int b) {
  if (!DebugOn(kDbgMark) || !Allow(kDbgMark)) return;
  RowBeginBare(g_tbl, 'K');
  char det[80];
  snprintf(det, sizeof(det), "%s a=%d b=0x%x", what, a, (unsigned)b);
  TableRowCell(g_tbl, det);
  TableRowEnd(g_tbl);
}

// Frameless text row (kDbgScreen): a guest console line read back from the
// video device. Linux prints only to VRAM (its `console=` came later), so the
// text mirror is the only way to read its console without a display window.
// A console line is up to 80 columns and the event table's DETAIL cell is 41
// wide — a truncated mirror cannot carry a call trace or a panic — so this one
// category prints whole lines instead of a table row.
void DebugText(const char* kind, const char* text) {
  if (!DebugOn(kDbgScreen) || !Allow(kDbgScreen)) return;
  char line[160];
  int n = snprintf(line, sizeof(line), "%s %s\n", kind, text);
  if (n < 0) return;
  if (DebugAccountOut((size_t)n)) HostWriteErr(line, (size_t)n);
}

void DebugSessionEnd(const frame* f, const char* stop_reason) {
  if (!g_inited) return;
  // Pure compat-trace mode stays byte-identical to the old interpreter's
  // output; the summary is a new-format feature.
  if (g_mask == kDbgTraceLine && !g_ndump) return;
  if (!(g_mask || g_nwatch || g_ndump)) return;
  if (g_mask & kDbgRegs) f->isa->dump_regs(f->cpu);
  char line[160];
  int n = snprintf(line, sizeof(line), "== session: inst=%llu traps=%llu",
                   (unsigned long long)f->cpu->inst_count, (unsigned long long)g_traps);
  for (int i = 0; i < 8; i++)
    if (g_suppressed[i])
      n += snprintf(line + n, sizeof(line) - (size_t)n, " supp[%d]=%llu", i,
                    (unsigned long long)g_suppressed[i]);
  snprintf(line + n, sizeof(line) - (size_t)n, " stop=%s\n", stop_reason);
  if (DebugAccountOut(strlen(line))) HostWriteErr(line, strlen(line));
  for (int i = 0; i < g_ndump; i++) WriteDump(i);
}
