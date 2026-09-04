// The debug hub: CEMU_DEBUG parsing, the event table, watchpoints, budget,
// and the session summary. Everything renders through util/table.c and
// HostWriteErr (streaming, ordered). Register naming goes through the ISA's
// DebugLayout so the core stays ISA-blind (AGENTS.md §X).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "debug/debug.h"
#include "bus/bus.h"
#include "cpu/step.h"
#include "host/host.h"
#include "util/table.h"
#include "util/log.h"

static uint32_t g_mask;
static int g_inited;
static int g_ascii = 1;      // plain borders by default (console codepages
                             // mangle UTF-8); `utf8` switches on
static int g_header_every = 40;
static int g_regs_period;      // 0 = off
static int g_budget = 100000;  // per-category event cap
static uint64_t g_skip = 0;  // per-category events suppressed BEFORE printing

// The one event table every category shares (AGENTS.md §X).
static const TableColumn kEventCols[] = {
    {"KIND", 4, kTableLeft},    {"PC", 16, kTableRight},
    {"RAW", 12, kTableLeft},    {"MNEMONIC", 19, kTableLeft},
    {"DETAIL", 41, kTableLeft}, {"FLAGS", 6, kTableLeft},
};
static Table *g_tbl;

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

// ---- init -------------------------------------------------------------------

static int ParseUint(const char *s, uint64_t *out) {
  char *end;
  *out = strtoull(s, &end, 0);
  return *s && *end == 0;
}

static void AddWatch(const char *spec) {
  // watch=ADDR:SIZE[:r|w|rw]
  if (g_nwatch >= kMaxWatches) return;
  uint64_t addr, size;
  const char *colon = strchr(spec, ':');
  if (!colon) return;
  char head[32];
  size_t hl = (size_t)(colon - spec);
  if (hl >= sizeof(head)) return;
  memcpy(head, spec, hl);
  head[hl] = 0;
  if (!ParseUint(head, &addr)) return;
  const char *rest = colon + 1;
  const char *colon2 = strchr(rest, ':');
  char szs[32];
  size_t sl = colon2 ? (size_t)(colon2 - rest) : strlen(rest);
  if (sl >= sizeof(szs)) return;
  memcpy(szs, rest, sl);
  szs[sl] = 0;
  if (!ParseUint(szs, &size)) return;
  int rw = 3;
  if (colon2) {
    if (!strcmp(colon2 + 1, "r")) rw = 1;
    else if (!strcmp(colon2 + 1, "w")) rw = 2;
  }
  g_watch[g_nwatch].addr = addr;
  g_watch[g_nwatch].size = (int)size;
  g_watch[g_nwatch].rw = rw;
  g_nwatch++;
}

void DebugInit(void) {
  g_inited = 1;
  const char *spec = getenv("CEMU_DEBUG");
  if (!spec) return;

  char buf[512];
  snprintf(buf, sizeof(buf), "%s", spec);
  char *save = NULL;
  for (char *tok = strtok_r(buf, ",", &save); tok;
       tok = strtok_r(NULL, ",", &save)) {
    if (!strcmp(tok, "trace") || !strcmp(tok, "trace:line"))
      g_mask |= kDbgTraceLine;
    else if (!strcmp(tok, "trace:table"))
      g_mask |= kDbgTraceTable;
    else if (!strcmp(tok, "state")) g_mask |= kDbgState;
    else if (!strcmp(tok, "mem") || !strcmp(tok, "mem:ld") ||
             !strcmp(tok, "mem:st"))
      g_mask |= kDbgMem;
    else if (!strcmp(tok, "trap")) g_mask |= kDbgTrap;
    else if (!strcmp(tok, "bus")) g_mask |= kDbgBus;
    else if (!strncmp(tok, "regs=", 5))
      g_regs_period = atoi(tok + 5);
    else if (!strncmp(tok, "watch=", 6))
      AddWatch(tok + 6);
    else if (!strncmp(tok, "budget=", 7))
      g_budget = atoi(tok + 7);
    else if (!strncmp(tok, "skip=", 5))
      g_skip = strtoull(tok + 5, NULL, 0);
    else if (!strcmp(tok, "utf8"))
      g_ascii = 0;
    else
      LogError("debug: unknown CEMU_DEBUG item '%s'", tok);
  }
  if (g_mask & (kDbgTraceTable | kDbgMem | kDbgTrap | kDbgBus) ||
      g_nwatch) {
    g_tbl = TableOpen(kEventCols, 6, g_ascii, g_header_every);
    TableHeader(g_tbl);
  }
}

int DebugOn(uint32_t cat) {
  return g_inited && ((g_mask & cat) || (g_nwatch && cat == kDbgMem));
}

// ---- rendering helpers ------------------------------------------------------

// Category index for the budget counters (bit position clamped into range).
static int CatIdx(uint32_t cat) {
  for (int i = 0; i < 8; i++)
    if (cat & (1u << i)) return i;
  return 7;
}

// Budget gate: 1 = emit, 0 = suppress (counted for the summary).
static int Allow(uint32_t cat) {
  int i = CatIdx(cat);
  if (g_emitted[i] < g_skip) {  // skip window: count, don't print
    g_emitted[i]++;
    return 0;
  }
  if (g_emitted[i] - g_skip < (uint64_t)g_budget) {
    g_emitted[i]++;
    return 1;
  }
  g_suppressed[i]++;
  return 0;
}

static void CellRaw(Table *t, const frame *f) {
  char c[32];
  int n = 0;
  for (int i = 0; i < f->rec.raw_len && i < 8; i++)
    n += snprintf(c + n, sizeof(c) - (size_t)n, "%02x ", f->rec.raw[i]);
  if (n) c[n - 1] = 0;  // trailing blank
  TableRowCell(t, c);
}

static void CellFlags(Table *t, frame *f) {
  if (f->isa->flag_names && f->isa->flag_word) {
    char c[32];
    uint64_t v = f->isa->flag_word(f);
    int n = 0;
    for (int i = 0; f->isa->flag_names[i] && n < (int)sizeof(c) - 6; i++)
      if (v & (1ULL << i))
        n += snprintf(c + n, sizeof(c) - (size_t)n, "%s ",
                      f->isa->flag_names[i]);
    if (n) c[n - 1] = 0;
    TableRowCell(t, n ? c : "-");
    return;
  }
  TableRowCell(t, "-");
}

// Rows emitted while fetch is still filling insn (the L lines of the fetch
// itself) must not read insn fields — pass NULL and blank them.
static void RowBeginBare(Table *t, char kind) {
  TableRowBegin(t);
  char k[2] = {kind, 0};
  TableRowCell(t, k);
  TableRowCell(t, "");
  TableRowCell(t, "");
  TableRowCell(t, "");
}

static void RowBegin(Table *t, char kind, frame *f) {
  TableRowBegin(t);
  char k[2] = {kind, 0};
  TableRowCell(t, k);
  char pc[20];
  snprintf(pc, sizeof(pc), "%016llx",
           (unsigned long long)f->rec.pc);
  TableRowCell(t, pc);
  CellRaw(t, f);
  TableRowCell(t, f->rec.mnemonic ? f->rec.mnemonic : "");
}

// ---- state stream -----------------------------------------------------------

// One canonical line per committed instruction: register banks in cell
// order (never mnemonic order) so the format survives refactors and is the
// diff baseline for golden runs (AGENTS.md §X).
static void StateRow(frame *f) {
  char line[1024];
  int n = snprintf(line, sizeof(line),
                   "S @pc=%016llx dnpc=%016llx | ",
                   (unsigned long long)f->rec.pc,
                   (unsigned long long)f->rec.dnpc);
  for (int bank = 0; bank < 2; bank++) {
    if (bank == 1 && !(f->isa->has_fpr && f->isa->has_fpr(f)))
      break;  // x86 has no FP bank in the stream
    for (int c = 0; c < 32 && n < (int)sizeof(line) - 24; c++) {
      uint64_t v = bank == 0 ? f->cpu->gpr[c] : f->cpu->fpr[c];
      n += snprintf(line + n, sizeof(line) - (size_t)n,
                    "%c%d=%016llx ", bank == 0 ? 'g' : 'f', c,
                    (unsigned long long)v);
    }
    n += snprintf(line + n, sizeof(line) - (size_t)n, "| ");
  }
  if (f->isa->debug_state_line)
    f->isa->debug_state_line(line + n, (int)sizeof(line) - n, f);
  HostWriteErr(line, (size_t)strlen(line));
  HostWriteErr("\n", 1);
}

// ---- emission points --------------------------------------------------------


void DebugInsn(frame *f) {
  if (!g_inited) return;
  if (g_mask & kDbgTraceLine) {
    // Compact one-line form: pc, raw bytes, mnemonic, next pc. Greppable,
    // no table chrome.
    if (Allow(kDbgTraceLine)) {
      char line[160];
      int n = snprintf(line, sizeof(line), "pc=%016llx",
                       (unsigned long long)f->rec.pc);
      for (int i = 0; i < f->rec.raw_len && i < 8; i++)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %02x",
                      f->rec.raw[i]);
      n += snprintf(line + n, sizeof(line) - (size_t)n, " %s -> %016llx\n",
                    f->rec.mnemonic ? f->rec.mnemonic : "?",
                    (unsigned long long)f->rec.dnpc);
      HostWriteErr(line, (size_t)n);
    }
  }
  if (g_mask & kDbgTraceTable) {
    if (Allow(kDbgTraceTable)) {
      RowBegin(g_tbl, 'I', f);
      char det[48];
      snprintf(det, sizeof(det), "-> %016llx",
               (unsigned long long)f->rec.dnpc);
      TableRowCell(g_tbl, det);
      CellFlags(g_tbl, f);
      TableRowEnd(g_tbl);
    }
  }
  if (g_mask & kDbgState) StateRow(f);
  if (g_regs_period &&
      (f->cpu->inst_count % (uint64_t)g_regs_period) == 0) {
    // Periodic register table reuses DumpRegs through a Frame-less path.
    f->isa->dump_regs(f->cpu);
  }
}

void DebugTrap(frame *f) {
  g_traps++;
  if (!DebugOn(kDbgTrap) || !Allow(kDbgTrap)) return;
  RowBegin(g_tbl, 'T', f);
  char det[96];
  const char *cn = f->isa->cause_name ? f->isa->cause_name(f->trap.cause) : NULL;
  snprintf(det, sizeof(det), "%s=%llu tval=%016llx -> %016llx",
           cn ? cn : "cause", (unsigned long long)f->trap.cause,
           (unsigned long long)f->trap.tval,
           (unsigned long long)f->rec.dnpc);
  TableRowCell(g_tbl, det);
  CellFlags(g_tbl, f);
  TableRowEnd(g_tbl);
}

void DebugMem(frame *f, uint64_t addr, int size, int acc,
              uint64_t val_or_result, int is_load) {
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
      snprintf(det, sizeof(det), "%s %llx:%d %s %llx",
               is_load ? "ld" : "st", (unsigned long long)addr, size,
               is_load ? "->" : "=",
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
    snprintf(det, sizeof(det), "%c %llx:%d %s %llx",
             is_load ? 'r' : 'w', (unsigned long long)addr, size,
             is_load ? "->" : "<-",
             (unsigned long long)(val_or_result & 0xffffffffULL));
    TableRowCell(g_tbl, det);
    CellFlags(g_tbl, f);
    TableRowEnd(g_tbl);
  }
}

void DebugBus(frame *f, const char *dev_name, uint64_t addr, int size,
              int is_load, uint64_t val) {
  if (!DebugOn(kDbgBus) || !Allow(kDbgBus)) return;
  RowBegin(g_tbl, 'B', f);
  char det[64];
  snprintf(det, sizeof(det), "%s %s:%llx:%d %s %llx", is_load ? "io-ld" : "io-st",
           dev_name ? dev_name : "?", (unsigned long long)addr, size,
           is_load ? "->" : "=",
           (unsigned long long)(val & 0xffffffffULL));
  TableRowCell(g_tbl, det);
  CellFlags(g_tbl, f);
  TableRowEnd(g_tbl);
}

void DebugSessionEnd(const frame *f, const char *stop_reason) {
  if (!g_inited) return;
  // Pure compat-trace mode stays byte-identical to the old interpreter's
  // output; the summary is a new-format feature.
  if (g_mask == kDbgTraceLine) return;
  if (!(g_mask || g_nwatch)) return;
  if (g_mask & kDbgRegs) f->isa->dump_regs(f->cpu);
  char line[160];
  int n = snprintf(line, sizeof(line),
                   "== session: inst=%llu traps=%llu",
                   (unsigned long long)f->cpu->inst_count,
                   (unsigned long long)g_traps);
  for (int i = 0; i < 8; i++)
    if (g_suppressed[i])
      n += snprintf(line + n, sizeof(line) - (size_t)n, " supp[%d]=%llu", i,
                    (unsigned long long)g_suppressed[i]);
  snprintf(line + n, sizeof(line) - (size_t)n, " stop=%s\n", stop_reason);
  HostWriteErr(line, (size_t)strlen(line));
}
