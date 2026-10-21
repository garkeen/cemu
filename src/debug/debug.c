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
// Per-category event cap, and the silent window each category counts through
// before it starts printing (AGENTS.md §X). One value, applied to every
// category's own counter: "per-category" is about the accounting, not about
// spelling the number once per category.
static int g_budget = 100000;
static uint64_t g_skip = 0;

// Hard session output cap (bytes). Every debug write is accounted here; once
// the cap is hit, further writes are dropped so a misconfigured CEMU_DEBUG
// can't fill the disk. ~5MB is plenty for any real diagnosis; raise it with a
// code change if you genuinely need more.
static const uint64_t kOutLimit = 5ULL * 1024 * 1024;
static uint64_t g_out_bytes;
static int g_limit_hit;

// The one event table every category shares (AGENTS.md §X). DETAIL carries a
// whole BIOS-call row ("gatecall int=21 ax=4800 bx=0400 cx=... at=9dbd:09de"),
// so it is wide enough for the longest of them: a column that truncates the
// field it was widened for is worse than a narrow one.
static const TableColumn kEventCols[] = {
    {"KIND", 4, kTableLeft},      {"PC", 16, kTableRight},    {"RAW", 12, kTableLeft},
    {"MNEMONIC", 19, kTableLeft}, {"DETAIL", 88, kTableLeft}, {"FLAGS", 6, kTableLeft},
};
static Table* g_tbl;

// One counter slot per category. Every kDbg* item is its own single bit, so
// the bit position is the slot index: scanning only the low eight bits filed
// kDbgRegs, kDbgGdb, kDbgMark and kDbgScreen into one shared slot, where they
// ate each other's budget and skip window (a `mark` run whose slot was spent
// by kDbgRegs' periodic register table stops printing marks, and `skip` for one
// of them silently moved the others' windows).
enum { kCatSlots = 16 };
static uint64_t g_emitted[kCatSlots];
static uint64_t g_suppressed[kCatSlots];
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

// ---- PC checkpoints (pcshot=ADDR[:COUNT]) ----------------------------------
// state= and trace= are indexed by retired-instruction count; REP loops eat a
// window's budget and timer I/O moves the count between runs, so neither can
// name "the state at this PC". The gdb stub can stop by linear PC but runs the
// whole boot through its per-step callback (several times slower than the
// interpreter). This item keeps the match in the ordinary fast path: after the
// commit, the frame's next linear PC is compared against the checkpoint, and
// the canonical state row is printed when the guest is about to execute that
// address. That is the boundary a QEMU gdb breakpoint stops on, so the two
// states compare field for field (AGENTS.md §十).
enum { kMaxPcShots = 8 };
static struct {
  uint64_t addr;
  uint64_t max;
  uint64_t hits;
} g_pcshot[kMaxPcShots];
static int g_npcshot;

// pcshot=ADDR[:COUNT]; ADDR is a C literal, COUNT defaults to 1.
static void AddPcShot(const char* spec) {
  if (g_npcshot >= kMaxPcShots) {
    LogError("debug: too many pcshots, dropping '%s'", spec);
    return;
  }
  const char* colon = strchr(spec, ':');
  size_t alen = colon ? (size_t)(colon - spec) : strlen(spec);
  char addr_text[32];
  if (alen == 0 || alen >= sizeof(addr_text)) {
    LogError("debug: pcshot expects ADDR[:COUNT] ('%s')", spec);
    return;
  }
  memcpy(addr_text, spec, alen);
  addr_text[alen] = 0;
  uint64_t addr = 0, max = 1;
  if (!ParseUint(addr_text, &addr)) {
    LogError("debug: pcshot address '%s' is not a number", addr_text);
    return;
  }
  if (colon && !ParseUint(colon + 1, &max)) {
    LogError("debug: pcshot count '%s' is not a number", colon + 1);
    return;
  }
  if (max == 0) {
    LogError("debug: pcshot count must be at least 1 ('%s')", spec);
    return;
  }
  g_pcshot[g_npcshot].addr = addr;
  g_pcshot[g_npcshot].max = max;
  g_pcshot[g_npcshot].hits = 0;
  g_npcshot++;
}

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

// ---- framebuffer dumps (fbdump=FILE) ----------------------------------------
// A rendered picture is not in guest memory: it is the display card's own
// XRGB buffer (device/video/display.h), which this hub does not own. The
// board installs the two accessors and the item is written at session end,
// beside dump=, in the same binary PPM (P6) QEMU's `screendump` writes, so
// the two pictures compare byte for byte. AGENTS.md §七 caps every file this
// repo writes at 5MB, so a picture over the cap is refused, not truncated.
enum { kMaxFbDumps = 2 };
static struct {
  char file[128];
} g_fbdump[kMaxFbDumps];
static int g_nfbdump;
static DebugFbInfoFn g_fb_info;
static DebugFbPixelsFn g_fb_pixels;
static void* g_fb_ctx;

void DebugSetFbReader(DebugFbInfoFn info, DebugFbPixelsFn pixels, void* ctx) {
  g_fb_info = info;
  g_fb_pixels = pixels;
  g_fb_ctx = ctx;
}

// ---- the clock frameless rows print -----------------------------------------
// The hub owns no CPU, so the board hands over the address of its retired
// instruction counter (debug/debug.h). Installed before DebugInit, which is what
// opens the session, so every row the session emits has a clock to read.
static const uint64_t* g_inst;

void DebugSetClock(const uint64_t* inst_count) { g_inst = inst_count; }

static void AddFbDump(const char* spec) {
  if (g_nfbdump >= kMaxFbDumps) {
    LogError("debug: too many framebuffer dumps, dropping '%s'", spec);
    return;
  }
  if (!*spec || strlen(spec) >= sizeof(g_fbdump[0].file)) {
    LogError("debug: fbdump expects a file name ('%s')", spec);
    return;
  }
  snprintf(g_fbdump[g_nfbdump].file, sizeof(g_fbdump[0].file), "%s", spec);
  g_nfbdump++;
}

// Writes the card's picture as a P6 PPM: the same header and the same
// top-to-bottom RGB order QEMU's screendump emits, so a diff of the two files
// is a diff of the two pictures. The pixel buffer is the card's own XRGB
// (0xffRRGGBB per display.h), converted to the PPM's RGB triples here.
static void WriteFbDump(const int idx) {
  const char* path = g_fbdump[idx].file;
  char line[192];
  // The board installs both accessors unconditionally (board/run.c), so
  // "no card" and "a card with nothing to show" are the same report: the card
  // this machine has, if any, publishes no picture.
  int w = 0, h = 0;
  if (!g_fb_info || !g_fb_pixels || !g_fb_info(g_fb_ctx, &w, &h) || w <= 0 || h <= 0) {
    LogError("debug: fbdump %s: this machine's display card has no picture", path);
    return;
  }
  const uint64_t pixels = (uint64_t)w * (uint64_t)h;
  const uint64_t rgb = pixels * 3;
  char hdr[32];
  const int hn = snprintf(hdr, sizeof(hdr), "P6\n%d %d\n255\n", w, h);
  if (rgb + (uint64_t)hn > kDumpLimit) {
    LogError("debug: fbdump %s: %dx%d needs %llu bytes, over the 5MB file cap", path, w, h,
             (unsigned long long)(rgb + (uint64_t)hn));
    return;
  }
  const uint32_t* px = g_fb_pixels(g_fb_ctx);
  if (!px) {
    LogError("debug: fbdump %s: the card has no framebuffer", path);
    return;
  }
  uint8_t* buf = (uint8_t*)malloc((size_t)(rgb + (uint64_t)hn));
  if (!buf) {
    LogError("debug: fbdump %s: out of memory", path);
    return;
  }
  memcpy(buf, hdr, (size_t)hn);
  uint8_t* out = buf + hn;
  for (uint64_t i = 0; i < pixels; i++) {
    const uint32_t p = px[i];
    out[i * 3 + 0] = (uint8_t)((p >> 16) & 0xff);
    out[i * 3 + 1] = (uint8_t)((p >> 8) & 0xff);
    out[i * 3 + 2] = (uint8_t)(p & 0xff);
  }
  HostFile* f = HostFileCreate(path);
  if (!f) {
    LogError("debug: fbdump %s: cannot create the file", path);
    free(buf);
    return;
  }
  const size_t total = (size_t)(rgb + (uint64_t)hn);
  const size_t wrote = HostFileWriteAt(f, 0, buf, total);
  HostFileClose(f);
  free(buf);
  int n = snprintf(line, sizeof(line), "== fbdump: %dx%d -> %s (%zu of %zu bytes)\n", w, h, path,
                   wrote, total);
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

enum { kMaxInj = 4 };
static struct {
  int kind;
  int dx, dy, dz, buttons;  // mouse
  uint32_t scan;            // key
  int extended, up;         // key
  uint64_t period;          // instructions between events
  uint64_t next_at;         // the instruction count of the next one
} g_inj[kMaxInj];
static int g_ninj;

int DebugNextInjection(uint64_t inst_count, DebugInjection* out) {
  for (int i = 0; i < g_ninj; i++) {
    if (inst_count < g_inj[i].next_at) continue;
    // Catch up in one step if the item came due while nobody was looking, so a
    // backlog does not arrive as a burst.
    do {
      g_inj[i].next_at += g_inj[i].period;
    } while (g_inj[i].next_at <= inst_count);
    out->kind = g_inj[i].kind;
    out->dx = g_inj[i].dx;
    out->dy = g_inj[i].dy;
    out->dz = g_inj[i].dz;
    out->buttons = g_inj[i].buttons;
    out->scan = g_inj[i].scan;
    out->extended = g_inj[i].extended;
    out->up = g_inj[i].up;
    if (DebugOn(kDbgMark))
      DebugMark(out->kind == kInjMouse ? "mouse-inj" : "key-inj",
                out->kind == kInjMouse ? out->dx : (int)out->scan,
                out->kind == kInjMouse ? out->dy : out->up);
    return 1;
  }
  return 0;
}

// The @N suffix every injection item carries: the period in instructions, which
// is required — a default would silently pick a cadence for the caller.
static int ParseInjectionPeriod(char* body, const char* spec, uint64_t* out) {
  char* at = strrchr(body, '@');
  if (!at) {
    LogError("debug: injection needs '@N' (the period in instructions): '%s'", spec);
    return 0;
  }
  *at = 0;
  char* end = NULL;
  long period = strtol(at + 1, &end, 0);
  if (!end || *end || period <= 0) {
    LogError("debug: injection period must be a positive number ('%s')", spec);
    return 0;
  }
  *out = (uint64_t)period;
  return 1;
}

// Both items are FIELD[:FIELD...]@N with signed C literals (hex needs 0x). A
// field that is not a number is an error: a silently dropped item would look
// like a probe that never gets its event.
static int ParseInjectionFields(char* body, const char* spec, long* vals, int want, int max) {
  int n = 0;
  char* save = NULL;
  for (char* f = strtok_r(body, ":", &save); f; f = strtok_r(NULL, ":", &save)) {
    if (n == max) break;
    char* end = NULL;
    vals[n] = strtol(f, &end, 0);
    if (!*f || !end || *end) {
      LogError("debug: injection field '%s' is not a number ('%s')", f, spec);
      return 0;
    }
    n++;
  }
  if (n < want) {
    LogError("debug: injection needs at least %d fields ('%s')", want, spec);
    return 0;
  }
  return n;
}

// mouse=DX:DY:BUTTONS[:WHEEL]@N
static void AddMouseInjection(const char* spec) {
  char body[128];
  if (strlen(spec) >= sizeof(body)) {
    LogError("debug: mouse= item too long ('%s')", spec);
    return;
  }
  strcpy(body, spec);
  uint64_t period = 0;
  if (!ParseInjectionPeriod(body, spec, &period)) return;
  long vals[4] = {0, 0, 0, 0};
  if (!ParseInjectionFields(body, spec, vals, 3, 4)) return;
  if (g_ninj >= kMaxInj) {
    LogError("debug: too many injection items, dropping '%s'", spec);
    return;
  }
  g_inj[g_ninj].kind = kInjMouse;
  g_inj[g_ninj].dx = (int)vals[0];
  g_inj[g_ninj].dy = (int)vals[1];
  g_inj[g_ninj].buttons = (int)vals[2];
  g_inj[g_ninj].dz = (int)vals[3];
  g_inj[g_ninj].period = period;
  g_inj[g_ninj].next_at = period;
  g_ninj++;
}

// key=SCAN[:EXT[:UP]]@N
static void AddKeyInjection(const char* spec) {
  char body[128];
  if (strlen(spec) >= sizeof(body)) {
    LogError("debug: key= item too long ('%s')", spec);
    return;
  }
  strcpy(body, spec);
  uint64_t period = 0;
  if (!ParseInjectionPeriod(body, spec, &period)) return;
  long vals[3] = {0, 0, 0};
  if (!ParseInjectionFields(body, spec, vals, 1, 3)) return;
  if (vals[0] < 0 || vals[0] > 0x7f) {
    LogError("debug: key= scan code must be 0..0x7f ('%s')", spec);
    return;
  }
  if (g_ninj >= kMaxInj) {
    LogError("debug: too many injection items, dropping '%s'", spec);
    return;
  }
  g_inj[g_ninj].kind = kInjKey;
  g_inj[g_ninj].scan = (uint32_t)vals[0];
  g_inj[g_ninj].extended = (int)vals[1];
  g_inj[g_ninj].up = (int)vals[2];
  g_inj[g_ninj].period = period;
  g_inj[g_ninj].next_at = period;
  g_ninj++;
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
    else if (!strcmp(tok, "vga-io"))
      g_mask |= kDbgVgaIo;
    else if (!strncmp(tok, "regs=", 5))
      g_regs_period = atoi(tok + 5);
    else if (!strncmp(tok, "watch=", 6))
      AddWatch(tok + 6);
    else if (!strncmp(tok, "dump=", 5))
      AddDump(tok + 5);
    else if (!strncmp(tok, "fbdump=", 7))
      AddFbDump(tok + 7);
    else if (!strncmp(tok, "pcshot=", 7)) {
      AddPcShot(tok + 7);
      g_mask |= kDbgPcShot;
    }
    else if (!strncmp(tok, "mouse=", 6))
      AddMouseInjection(tok + 6);
    else if (!strncmp(tok, "key=", 4))
      AddKeyInjection(tok + 4);
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
          (kDbgTraceTable | kDbgMem | kDbgTrap | kDbgBus | kDbgGdb | kDbgMark |
           kDbgScreen | kDbgVgaIo) ||
      g_nwatch) {
    g_tbl = TableOpen(kEventCols, 6, g_ascii, g_header_every);
    TableHeader(g_tbl);
  }
}

int DebugOn(uint32_t cat) { return g_inited && ((g_mask & cat) || (g_nwatch && cat == kDbgMem)); }

// ---- rendering helpers ------------------------------------------------------

// Category index for the budget counters (bit position clamped into range).
static int CatIdx(uint32_t cat) {
  for (int i = 0; i < kCatSlots; i++)
    if (cat & (1u << i)) return i;
  return kCatSlots - 1;
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
// itself) must not read insn fields — pass NULL and blank the raw/mnemonic
// cells. The PC cell is not blank: a frameless row is only useful if it can be
// placed on the guest's own clock, and `inst` is what makes a checkpoint
// reproducible as --max-inst=<n> (whose session end then brings the guest state
// back through dump=).
static void RowBeginBare(Table* t, char kind) {
  TableRowBegin(t);
  char k[2] = {kind, 0};
  TableRowCell(t, k);
  char pc[20];
  snprintf(pc, sizeof(pc), "%016llx", (unsigned long long)*g_inst);
  TableRowCell(t, pc);
  TableRowCell(t, "");
  TableRowCell(t, "");
}

// The address this step is fetching from, in the linear space gdb and the
// memory packets name. An ISA with a gdb_pc hook records the fetch-time linear
// address in rec.pc_lin (x86: the CS base before the instruction ran, which is
// not the base after a far jump); one without one is already linear.
static uint64_t FramePc(frame* f) {
  return f->isa->gdb_pc ? f->rec.pc_lin : f->rec.pc;
}

// The next instruction's linear address, recorded at commit in the segment the
// CPU is in after the step. This must be the same convention as FramePc, or
// one row's `->` would not be the next row's `pc=`.
static uint64_t FrameDnpc(frame* f) {
  return f->isa->gdb_pc ? f->rec.dnpc_lin : f->rec.dnpc;
}

static void RowBegin(Table* t, char kind, frame* f) {
  TableRowBegin(t);
  char k[2] = {kind, 0};
  TableRowCell(t, k);
  char pc[20];
  snprintf(pc, sizeof(pc), "%016llx", (unsigned long long)FramePc(f));
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
      int n = snprintf(line, sizeof(line), "pc=%016llx", (unsigned long long)FramePc(f));
      for (int i = 0; i < f->rec.raw_len && i < 8; i++)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %02x", f->rec.raw[i]);
      n += snprintf(line + n, sizeof(line) - (size_t)n, " %s -> %016llx\n",
                    f->rec.mnemonic ? f->rec.mnemonic : "?", (unsigned long long)FrameDnpc(f));
      if (DebugAccountOut((size_t)n)) HostWriteErr(line, (size_t)n);
    }
  }
  if (g_mask & kDbgTraceTable) {
    if (AllowTrace(kDbgTraceTable, f->cpu->inst_count)) {
      RowBegin(g_tbl, 'I', f);
      char det[48];
      snprintf(det, sizeof(det), "-> %016llx", (unsigned long long)FrameDnpc(f));
      TableRowCell(g_tbl, det);
      CellFlags(g_tbl, f);
      TableRowEnd(g_tbl);
    }
  }
  // The state stream is a category like any other: it spends its own budget
  // and its own skip window, and the session summary reports what it
  // suppressed. It used to bypass both, which is how one `state,skip=N` run
  // printed the whole stream and ran into the 5MB output cap.
  if ((g_mask & kDbgState) && Allow(kDbgState)) StateRow(f);
  // PC checkpoints are the PC-indexed counterpart of the state window: match
  // the boundary the commit lands on (next linear PC), then print the same
  // canonical row, so a gdb stop at that address in the reference model has a
  // field-for-field counterpart here. Not budgeted: a checkpoint is a fixed
  // number of rows chosen by the caller, and the hard output cap still holds.
  if (g_mask & kDbgPcShot) {
    uint64_t next = FrameDnpc(f);
    for (int i = 0; i < g_npcshot; i++) {
      if (g_pcshot[i].hits < g_pcshot[i].max && g_pcshot[i].addr == next) {
        char mark[96];
        g_pcshot[i].hits++;
        int n = snprintf(mark, sizeof(mark), "P @lin=%016llx hit=%llu inst=%llu\n",
                         (unsigned long long)next,
                         (unsigned long long)g_pcshot[i].hits,
                         (unsigned long long)f->cpu->inst_count);
        if (DebugAccountOut((size_t)n)) HostWriteErr(mark, (size_t)n);
        StateRow(f);
      }
    }
  }
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
           (unsigned long long)FrameDnpc(f));
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

// See debug.h: a software INT n gets one frameless row per half, like every
// other mark row but wider than DebugMark's two numbers, because a call's
// parameters and its answer do not reduce to two. `caller` is (cs << 16) | ip,
// the same packing the gate row uses for a vector's target, and it is the key
// the two halves pair on.
//
// A software INT n's request row: which vector, and the registers the service
// reads as its parameters. All of them, because which one matters is the
// service's choice -- AH=48h takes its size in BX, AH=3Dh its filename in DS:DX
// -- and a row carrying only AX can report "out of memory" without saying how
// much was asked for, which is the question the guest came to answer.
void DebugGateCall(uint32_t vec, const DbgCallArgs* a, uint32_t caller) {
  if (!DebugOn(kDbgMark) || !Allow(kDbgMark)) return;
  RowBeginBare(g_tbl, 'K');
  char det[104];
  snprintf(det, sizeof(det),
           "gatecall int=%02x ax=%04x bx=%04x cx=%04x dx=%04x ds=%04x es=%04x at=%04x:%04x",
           vec, a->arg_ax & 0xffff, a->arg_bx & 0xffff, a->arg_cx & 0xffff, a->arg_dx & 0xffff,
           a->arg_ds & 0xffff, a->arg_es & 0xffff, (caller >> 16) & 0xffff, caller & 0xffff);
  TableRowCell(g_tbl, det);
  TableRowEnd(g_tbl);
}

// The answer half. BX/CX/DX are here because a handler returns through them too
// (int 10h AH=0Fh answers the column count in AH and the page in BH; DOS
// AH=45h answers with the duplicated handle in AX and AH=44h/0Dh uses BX); the
// caller's segments are not, since a handler that changed them changed the
// caller's context rather than answering anything.
void DebugGateRet(const DbgCallArgs* a, int cf, uint32_t caller) {
  if (!DebugOn(kDbgMark) || !Allow(kDbgMark)) return;
  RowBeginBare(g_tbl, 'K');
  char det[80];
  snprintf(det, sizeof(det), "gateret ax=%04x bx=%04x cx=%04x dx=%04x cf=%d at=%04x:%04x",
           a->arg_ax & 0xffff, a->arg_bx & 0xffff, a->arg_cx & 0xffff, a->arg_dx & 0xffff, cf,
           (caller >> 16) & 0xffff, caller & 0xffff);
  TableRowCell(g_tbl, det);
  TableRowEnd(g_tbl);
}

// Guest port access to the card's register file (kDbgVgaIo). Both numbers are
// hex: a port number and a register value are both read as masks and addresses,
// and the trace this mirrors (QEMU's vga_std_read_io / vga_std_write_io) prints
// them that way too -- so the two transcripts line up without a translation step.
// The direction is in the row because a read and a write are different
// questions: the write asks the card to change, the read asks it what it thinks.
void DebugVgaIo(uint32_t port, uint32_t val, int is_read) {
  if (!DebugOn(kDbgVgaIo) || !Allow(kDbgVgaIo)) return;
  RowBeginBare(g_tbl, 'K');
  char det[80];
  snprintf(det, sizeof(det), "vga-io %s port=0x%03x val=0x%02x",
           is_read ? "rd" : "wr", port, val & 0xff);
  TableRowCell(g_tbl, det);
  TableRowEnd(g_tbl);
}

// Frameless text row (kDbgScreen): a guest console line read back from the
// video device. Linux prints only to VRAM (its `console=` came later), so the
// text mirror is the only way to read its console without a display window.
// A console line is 80 columns of the guest's own text, with nothing to put in
// the table's PC/RAW/MNEMONIC/FLAGS cells, so this category prints whole lines
// rather than a row of chrome around them.
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
  if (g_mask == kDbgTraceLine && !g_ndump && !g_nfbdump) return;
  if (!(g_mask || g_nwatch || g_ndump || g_nfbdump)) return;
  if (g_mask & kDbgRegs) f->isa->dump_regs(f->cpu);
  char line[160];
  int n = snprintf(line, sizeof(line), "== session: inst=%llu traps=%llu",
                   (unsigned long long)f->cpu->inst_count, (unsigned long long)g_traps);
  for (int i = 0; i < kCatSlots; i++)
    if (g_suppressed[i])
      n += snprintf(line + n, sizeof(line) - (size_t)n, " supp[%d]=%llu", i,
                    (unsigned long long)g_suppressed[i]);
  snprintf(line + n, sizeof(line) - (size_t)n, " stop=%s\n", stop_reason);
  if (DebugAccountOut(strlen(line))) HostWriteErr(line, strlen(line));
  for (int i = 0; i < g_ndump; i++) WriteDump(i);
  for (int i = 0; i < g_nfbdump; i++) WriteFbDump(i);
}
