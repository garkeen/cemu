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
// Hot-path gates the interpreters read directly (see debug.h): whether any
// per-access/per-instruction observation is on, whether raw instruction bytes
// are needed, and whether memory accesses must take the event-emitting slow
// path instead of the TLB/宿主指针 fast path.
int g_mem_slow;
int g_record_raw;
int g_dbg_any;
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
enum { kCatSlots = 18 };
static uint64_t g_emitted[kCatSlots];
static uint64_t g_suppressed[kCatSlots];
static uint64_t g_traps;
static int g_trap_cause = -1;  // trap=CAUSE: only this vector prints
static char g_mark_name[24];   // mark:NAME: only this mark name prints
static int g_mark_vec = -1;    // mark:gatecall=N: only this INT vector
static int g_mark_ah = -1;     // mark:gatecall=N:AH: only this INT function
static int g_last_gate_vec = -1;  // vector of the call the pending return answers
static int g_last_gate_ah = -1;   // AH of that call (the paired-function filter)
static char g_screen_stop[24];  // screenstop=TEXT: end the session on that line

// ---- the instruction in progress (DebugSetFrame) ---------------------------
// One pointer store per step, the same shape as QEMU's global current CPU: it
// is what lets a row with no instruction frame still name the guest code that
// caused it (debug.h).
static frame* g_frame;
enum { kHistDefault = 32, kHistMax = 64 };
static struct {
  uint64_t pc;
  const char* mnem;
  uint8_t raw[8];
  uint8_t raw_len;
} g_hist[kHistMax];
static int g_hist_len;  // ring depth from `hist[=N]`; 0 = category off
static int g_hist_at;
static int g_hist_filled;

// ---- `bus:ADDR` / `bus:NAME` -----------------------------------------------
// Unfiltered, the bus category opens with a boot's ROM shadow copy: tens of
// thousands of rows before the guest's first interesting access, and the
// session output cap then eats the window the diagnosis needed. Filtering is
// what QEMU's -trace event patterns do for its own log.
enum { kBusAll = 0, kBusAddr = 1, kBusName = 2 };
static int g_bus_kind;
static uint64_t g_bus_addr;
static char g_bus_name[40];

static int BusWanted(const char* dev_name, uint64_t addr) {
  if (g_bus_kind == kBusAddr) return addr == g_bus_addr;
  if (g_bus_kind == kBusName) return dev_name && !strcmp(dev_name, g_bus_name);
  return 1;
}

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

// Linear watchpoints (watchl=): the same match, but on the address the guest
// names instead of the one the bus sees. `watch=` is physical because the bus
// funnels only know the translated address; a protected-mode structure (a VxD
// data slot, a kernel object) has no stable physical address across runs --
// paging decides it -- so "who wrote this slot" cannot be asked there. The
// match is made in the ISA's linear funnel (cpu/isa/x86/exec.c lin_load/
// lin_store) while `g_mem_slow` is on, which a linear watch forces exactly the
// way a physical one does.
static struct {
  uint64_t addr;
  int size;
  int rw;  // 1 read, 2 write, 3 both
} g_watchl[kMaxWatches];
static int g_nwatchl;

int DebugLinWatchHit(uint64_t addr, int size, int is_load) {
  int want = is_load ? 1 : 2;
  for (int i = 0; i < g_nwatchl; i++)
    if (g_watchl[i].rw & want && addr < g_watchl[i].addr + (uint64_t)g_watchl[i].size &&
        addr + (uint64_t)size > g_watchl[i].addr)
      return 1;
  return 0;
}

// ---- exec= : the fetch that crosses into a linear range --------------------
// A watch cannot answer this: the region may be read as data thousands of
// times before the guest executes from it, and `pcshot=` fires on every
// landing inside it rather than on the crossing. This item matches FETCHES
// only (acc == 0 in the memory funnel), keeps one bit of state -- was the
// previous fetch inside a range -- and reports the first fetch of each entry.
// That is the crossing: the caller gets the history ring (the jump that did
// it) and the landing point, which is what "who sent the control flow into
// this memory" asks (AGENTS.md §九 rule 9). The region a runaway lands in is
// data the guest cleared, so no address in it can be guessed from the map.
enum { kMaxExec = 4 };
static struct {
  uint64_t lo;   // inclusive
  uint64_t hi;   // exclusive
  uint32_t max;  // crossing rows this range reports (the optional :COUNT)
  uint32_t hits;
} g_exec[kMaxExec];
static int g_nexec;
static int g_fetch_inside;  // the previous fetch was inside one of the ranges

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

// ---- PC checkpoints (pcshot=ADDR[:COUNT], pcshot=LO-HI[:COUNT]) ------------
// state= and trace= are indexed by retired-instruction count; REP loops eat a
// window's budget and timer I/O moves the count between runs, so neither can
// name "the state at this PC". The gdb stub can stop by linear PC but runs the
// whole boot through its per-step callback (several times slower than the
// interpreter). This item keeps the match in the ordinary fast path: after the
// commit, the frame's next linear PC is compared against the checkpoint, and
// the canonical state row is printed when the guest is about to execute that
// address. That is the boundary a QEMU gdb breakpoint stops on, so the two
// states compare field for field (AGENTS.md §十). LO-HI names a range instead
// of one address: "the first fetch inside this region" is what a runaway
// control flow asks, and a guessed address cannot answer it (AGENTS.md §九
// rule 9). A hit also dumps the history ring when `hist` is on, because the
// jump that lands in the region is the row before the landing.
enum { kMaxPcShots = 8 };
static struct {
  uint64_t lo;  // inclusive
  uint64_t hi;  // exclusive
  uint64_t max;
  uint64_t hits;
} g_pcshot[kMaxPcShots];
static int g_npcshot;

// pcshot=ADDR[:COUNT], pcshot=LO-HI[:COUNT]; the address parts are C literals,
// COUNT defaults to 1. '-' never occurs inside a C literal, so the range form
// is unambiguous.
static void AddPcShot(const char* spec) {
  if (g_npcshot >= kMaxPcShots) {
    LogError("debug: too many pcshots, dropping '%s'", spec);
    return;
  }
  const char* colon = strchr(spec, ':');
  size_t alen = colon ? (size_t)(colon - spec) : strlen(spec);
  char addr_text[64];
  if (alen == 0 || alen >= sizeof(addr_text)) {
    LogError("debug: pcshot expects ADDR[:COUNT] or LO-HI[:COUNT] ('%s')", spec);
    return;
  }
  memcpy(addr_text, spec, alen);
  addr_text[alen] = 0;
  uint64_t lo = 0, hi = 0, max = 1;
  char* dash = strchr(addr_text, '-');
  if (dash) {
    *dash = 0;
    if (!ParseUint(addr_text, &lo) || !ParseUint(dash + 1, &hi)) {
      LogError("debug: pcshot range '%s' is not a pair of numbers", spec);
      return;
    }
    if (hi <= lo) {
      LogError("debug: pcshot range needs LO < HI ('%s')", spec);
      return;
    }
  } else {
    if (!ParseUint(addr_text, &lo)) {
      LogError("debug: pcshot address '%s' is not a number", addr_text);
      return;
    }
    hi = lo + 1;
  }
  if (colon && !ParseUint(colon + 1, &max)) {
    LogError("debug: pcshot count '%s' is not a number", colon + 1);
    return;
  }
  if (max == 0) {
    LogError("debug: pcshot count must be at least 1 ('%s')", spec);
    return;
  }
  g_pcshot[g_npcshot].lo = lo;
  g_pcshot[g_npcshot].hi = hi;
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
  // Own category: hits obey `budget=`/`skip=` like every other event kind.
  // Unbudgeted, a watch over a region the guest copies through fills the 5MB
  // output cap before the crossing the caller asked about.
  g_mask |= kDbgWatch;
}

static void AddWatchL(const char* spec) {
  // watchl=ADDR:SIZE[:r|w|rw] -- the linear-address twin of watch=.
  if (g_nwatchl >= kMaxWatches) {
    LogError("debug: too many linear watches, dropping '%s'", spec);
    return;
  }
  uint64_t addr, size;
  const char* colon = strchr(spec, ':');
  if (!colon) {
    LogError("debug: watchl expects ADDR:SIZE[:r|w|rw] ('%s')", spec);
    return;
  }
  char head[32];
  size_t hl = (size_t)(colon - spec);
  if (hl >= sizeof(head)) return;
  memcpy(head, spec, hl);
  head[hl] = 0;
  if (!ParseUint(head, &addr)) {
    LogError("debug: watchl address must be a number, 0x-prefixed for hex ('%s')", head);
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
    LogError("debug: watchl size must be a number ('%s')", szs);
    return;
  }
  int rw = 3;
  if (colon2) {
    if (!strcmp(colon2 + 1, "r"))
      rw = 1;
    else if (!strcmp(colon2 + 1, "w"))
      rw = 2;
  }
  g_watchl[g_nwatchl].addr = addr;
  g_watchl[g_nwatchl].size = (int)size;
  g_watchl[g_nwatchl].rw = rw;
  g_nwatchl++;
  g_mask |= kDbgWatch;
}

// exec=LO-HI (repeatable, up to kMaxExec); LO and HI are C literals.
static void AddExec(const char* spec) {
  if (g_nexec >= kMaxExec) {
    LogError("debug: too many exec ranges, dropping '%s'", spec);
    return;
  }
  char text[64];
  size_t n = strlen(spec);
  if (n == 0 || n >= sizeof(text)) {
    LogError("debug: exec expects LO-HI ('%s')", spec);
    return;
  }
  memcpy(text, spec, n + 1);
  // The optional :COUNT suffix belongs to the range, not to HI: leaving it on
  // made ParseUint reject the whole spec, so `exec=LO-HI:N` — the documented
  // form — armed nothing at all while the error line scrolled past.
  uint32_t max = 1;
  char* colon = strrchr(text, ':');
  if (colon) {
    uint64_t v = 0;
    *colon = 0;
    if (!ParseUint(colon + 1, &v) || v == 0 || v > 0xffff) {
      LogError("debug: exec count must be 1..65535 ('%s')", spec);
      return;
    }
    max = (uint32_t)v;
  }
  char* dash = strchr(text, '-');
  if (!dash) {
    LogError("debug: exec expects LO-HI ('%s')", spec);
    return;
  }
  *dash = 0;
  uint64_t lo = 0, hi = 0;
  if (!ParseUint(text, &lo) || !ParseUint(dash + 1, &hi)) {
    LogError("debug: exec range '%s' is not a pair of numbers", spec);
    return;
  }
  if (hi <= lo) {
    LogError("debug: exec range needs LO < HI ('%s')", spec);
    return;
  }
  g_exec[g_nexec].lo = lo;
  g_exec[g_nexec].hi = hi;
  g_exec[g_nexec].max = max;
  g_exec[g_nexec].hits = 0;
  g_nexec++;
  g_mask |= kDbgExec;
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
    else if (!strncmp(tok, "trap=", 5)) {
      // trap=CAUSE: one vector only. A guard-triggered storm (the #UD a
      // mis-delivered V86/PM frame causes) buries the first hit under
      // millions of repeats; with the vector the window opens on the first.
      uint64_t v = 0;
      if (!ParseUint(tok + 5, &v) || v > 0xff) {
        LogError("debug: trap= wants a vector number ('%s')", tok);
      } else {
        g_trap_cause = (int)v;
        g_mask |= kDbgTrap;
      }
    }
    else if (!strcmp(tok, "bus") || !strncmp(tok, "bus:", 4)) {
      g_mask |= kDbgBus;
      if (tok[3] == ':') {
        uint64_t v = 0;
        if (ParseUint(tok + 4, &v)) {
          g_bus_kind = kBusAddr;
          g_bus_addr = v;
        } else {
          g_bus_kind = kBusName;
          snprintf(g_bus_name, sizeof(g_bus_name), "%s", tok + 4);
        }
      }
    }
    else if (!strcmp(tok, "hist") || !strncmp(tok, "hist=", 5)) {
      g_mask |= kDbgHist;
      g_hist_len = tok[4] == '=' ? atoi(tok + 5) : kHistDefault;
      if (g_hist_len < 1) g_hist_len = 1;
      if (g_hist_len > kHistMax) g_hist_len = kHistMax;
    }
    else if (!strcmp(tok, "gdb"))
      g_mask |= kDbgGdb;
    else if (!strcmp(tok, "mark"))
      g_mask |= kDbgMark;
    else if (!strncmp(tok, "mark:", 5)) {
      // mark:NAME[=N] -- one mark name only (gatecall/gateret, intr/inta,
      // fdc-* ...), and for the gate rows one vector. `mark` is a catch-all:
      // an interrupt line storm, or a keyboard poll loop once the guest is
      // waiting for a key, buries the one stream the caller is after.
      char* eq = strchr(tok + 5, '=');
      size_t n = eq ? (size_t)(eq - (tok + 5)) : strlen(tok + 5);
      if (n == 0 || n >= sizeof(g_mark_name)) {
        LogError("debug: mark name too long ('%s')", tok);
      } else {
        memcpy(g_mark_name, tok + 5, n);
        g_mark_name[n] = 0;
        g_mark_vec = -1;
        g_mark_ah = -1;
        if (eq) {
          // gatecall=N selects one vector; the optional :AH then selects one
          // DOS/BIOS function inside it (INT 21h AH=3Fh is "read", and a file
          // load's reads are what a failing VxD loader loses).
          char spec[32];
          snprintf(spec, sizeof(spec), "%s", eq + 1);
          char* colon = strchr(spec, ':');
          if (colon) *colon = 0;
          uint64_t v = 0;
          if (!ParseUint(spec, &v) || v > 0xff)
            LogError("debug: mark vector must be 0..255 ('%s')", tok);
          else {
            g_mark_vec = (int)v;
            if (colon) {
              uint64_t ah = 0;
              if (!ParseUint(colon + 1, &ah) || ah > 0xff)
                LogError("debug: mark function must be 0..255 ('%s')", tok);
              else
                g_mark_ah = (int)ah;
            }
          }
        }
        g_mask |= kDbgMark;
      }
    }
    else if (!strcmp(tok, "pmp"))
      g_mask |= kDbgPmp;
    else if (!strcmp(tok, "virtio"))
      g_mask |= kDbgVirtio;
    else if (!strcmp(tok, "screen"))
      g_mask |= kDbgScreen;
    else if (!strncmp(tok, "screenstop=", 11)) {
      // The transcript a failure leaves behind is text: stop on it and let
      // dump=/regs write the post-mortem. Needs `screen` to see the line.
      if (strlen(tok + 11) >= sizeof(g_screen_stop))
        LogError("debug: screenstop text too long ('%s')", tok);
      else {
        snprintf(g_screen_stop, sizeof(g_screen_stop), "%s", tok + 11);
        g_mask |= kDbgScreen;
      }
    }
    else if (!strcmp(tok, "vga-io"))
      g_mask |= kDbgVgaIo;
    else if (!strncmp(tok, "regs=", 5)) {
      g_regs_period = atoi(tok + 5);
      // The period alone would never be tested: DebugInsn only runs when a
      // category (or a watchpoint) is on, and this bit also arms the exit dump.
      g_mask |= kDbgRegs;
    }
    else if (!strncmp(tok, "watchl=", 7))
      AddWatchL(tok + 7);
    else if (!strncmp(tok, "watch=", 6))
      AddWatch(tok + 6);
    else if (!strncmp(tok, "exec=", 5))
      AddExec(tok + 5);
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
           kDbgScreen | kDbgVgaIo | kDbgPmp | kDbgVirtio | kDbgExec) ||
      g_nwatch || g_nwatchl || g_nexec) {
    g_tbl = TableOpen(kEventCols, 6, g_ascii, g_header_every);
    TableHeader(g_tbl);
  }
  // One-time resolution of the per-access/per-instruction gates. The hot
  // interpreters test these plain globals instead of calling DebugOn on every
  // byte of every instruction; a category that needs the event-emitting path
  // disables the fast memory path wholesale.
  g_mem_slow = ((g_mask & (kDbgMem | kDbgBus)) != 0) || g_nwatch != 0 || g_nwatchl != 0;
  g_record_raw = (g_mask & (kDbgTraceLine | kDbgTraceTable | kDbgHist)) != 0;
  g_dbg_any = (g_mask != 0) || g_nwatch != 0 || g_nwatchl != 0;
  // One line that says the range item armed: a range that parsed to nothing
  // used to be indistinguishable from one that never saw a crossing.
  if (g_nexec) LogInfo("debug: %d exec range(s) armed", g_nexec);
  if (g_npcshot) LogInfo("debug: %d pc checkpoint(s) armed", g_npcshot);
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

// ---- the instruction in progress, and the ones before it --------------------
// Definitions; the contract and the reasoning live in debug.h.

void DebugSetFrame(frame* f) { g_frame = f; }

// " pc=<linear>": the address of the instruction the machine was running when
// the event fired, appended to a row that has no frame of its own.
static void AppendCtx(char* det, size_t cap) {
  if (!g_frame) return;
  size_t n = strlen(det);
  if (n + 24 >= cap) return;
  snprintf(det + n, cap - n, " pc=%llx", (unsigned long long)FramePc(g_frame));
}

// Ring record, called after the commit (DebugInsn) rather than from
// DebugSetFrame: the frame published at fetch time still has no mnemonic and no
// instruction bytes, and a ring of blank entries answers nothing.
static void HistRecord(frame* f) {
  if (!g_hist_len) return;
  int slot = g_hist_at;
  int n = f->rec.raw_len;
  if (n > (int)sizeof(g_hist[slot].raw)) n = (int)sizeof(g_hist[slot].raw);
  // A REP instruction retires once per iteration, so a `rep movsd` fills the
  // whole ring with copies of itself and the call chain -- the only reason the
  // dump exists -- is lost. A run of identical consecutive entries collapses
  // to one. Free unless `hist` is on.
  if (g_hist_filled) {
    int last = (slot + g_hist_len - 1) % g_hist_len;
    if (g_hist[last].pc == FramePc(f) && g_hist[last].raw_len == (uint8_t)n &&
        memcmp(g_hist[last].raw, f->rec.raw, (size_t)n) == 0)
      return;
  }
  g_hist[slot].pc = FramePc(f);
  g_hist[slot].mnem = f->rec.mnemonic;
  g_hist[slot].raw_len = (uint8_t)n;
  for (int i = 0; i < n; i++) g_hist[slot].raw[i] = f->rec.raw[i];
  g_hist_at = (slot + 1) % g_hist_len;
  if (g_hist_filled < g_hist_len) g_hist_filled++;
}

// The ring, oldest first, one plain line each: a detail cell cannot carry a
// chain, and the chain is what says whether an event came from the guest's own
// code or from an interrupt taken in the middle of something else.
static void HistDump(void) {
  if (!(g_mask & kDbgHist) || !g_hist_filled) return;
  // No window of its own: the ring rides on rows the caller already admitted, so
  // a `skip=N` of its own could never fill (the rows it would count were
  // suppressed before reaching here, and the dump then stayed silent for the
  // whole run). The budget still applies, and the counters stay in this
  // category's own slot so the session summary reports it honestly.
  int slot_i = CatIdx(kDbgHist);
  if (g_emitted[slot_i] >= (uint64_t)g_budget) {
    g_suppressed[slot_i]++;
    return;
  }
  g_emitted[slot_i]++;
  for (int i = 0; i < g_hist_filled; i++) {
    int slot = ((g_hist_at - g_hist_filled + i) % g_hist_len + g_hist_len) % g_hist_len;
    char line[128];
    int n = snprintf(line, sizeof(line), "hist %016llx %-10s",
                     (unsigned long long)g_hist[slot].pc,
                     g_hist[slot].mnem ? g_hist[slot].mnem : "-");
    for (int b = 0; b < g_hist[slot].raw_len; b++)
      n += snprintf(line + n, sizeof(line) - (size_t)n, " %02x", g_hist[slot].raw[b]);
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    line[n] = 0;
    if (DebugAccountOut((size_t)n)) HostWriteErr(line, (size_t)n);
  }
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
  if (g_mask & kDbgHist) HistRecord(f);
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
  // field-for-field counterpart here. Budgeted through this item's own
  // category: a range the guest executes from the start of the boot is hit
  // thousands of times before the crossing that matters, and `skip=` names
  // that crossing without a guessed instruction count.
  if (g_mask & kDbgPcShot) {
    uint64_t next = FrameDnpc(f);
    for (int i = 0; i < g_npcshot; i++) {
      if (g_pcshot[i].hits < g_pcshot[i].max && next >= g_pcshot[i].lo &&
          next < g_pcshot[i].hi) {
        char mark[96];
        g_pcshot[i].hits++;
        if (!Allow(kDbgPcShot)) continue;
        int n = snprintf(mark, sizeof(mark), "P @lin=%016llx hit=%llu inst=%llu\n",
                         (unsigned long long)next,
                         (unsigned long long)g_pcshot[i].hits,
                         (unsigned long long)f->cpu->inst_count);
        if (DebugAccountOut((size_t)n)) HostWriteErr(mark, (size_t)n);
        StateRow(f);
        HistDump();
      }
    }
  }
  // exec= : the crossing into a range, judged on the same boundary as the
  // checkpoints (the next linear PC). The whole item lives inside this
  // already-gated per-step hook, so a run without it pays nothing in the fetch
  // path (AGENTS.md §九 rule 10). One row per crossing: a runaway that walks
  // cleared memory stays inside the range and reports nothing more, and the row
  // carries the instruction that produced the crossing.
  if (g_nexec) {
    uint64_t next = FrameDnpc(f);
    int inside = 0;
    int report = 0;
    for (int i = 0; i < g_nexec; i++)
      if (next >= g_exec[i].lo && next < g_exec[i].hi) {
        inside = 1;
        // :COUNT caps the rows this range reports, not the crossings it sees.
        if (g_exec[i].hits < g_exec[i].max) {
          g_exec[i].hits++;
          report = 1;
        }
        break;
      }
    if (inside && report && !g_fetch_inside && Allow(kDbgExec)) {
      char det[72];
      snprintf(det, sizeof(det), "enter %016llx inst=%llu", (unsigned long long)next,
               (unsigned long long)f->cpu->inst_count);
      RowBegin(g_tbl, 'X', f);
      TableRowCell(g_tbl, det);
      CellFlags(g_tbl, f);
      TableRowEnd(g_tbl);
      HistDump();
    }
    g_fetch_inside = inside;
  }
  if (g_regs_period && (f->cpu->inst_count % (uint64_t)g_regs_period) == 0) {
    // Periodic register table reuses DumpRegs through a Frame-less path.
    f->isa->dump_regs(f->cpu);
  }
}

void DebugTrap(frame* f) {
  g_traps++;
  if (g_trap_cause >= 0 && (int)f->trap.cause != g_trap_cause) return;
  if (!DebugOn(kDbgTrap) || !Allow(kDbgTrap)) return;
  RowBegin(g_tbl, 'T', f);
  char det[128];
  const char* cn = f->isa->cause_name ? f->isa->cause_name(f->trap.cause) : NULL;
  int n = snprintf(det, sizeof(det), "%s=%llu tval=%016llx", cn ? cn : "cause",
                   (unsigned long long)f->trap.cause, (unsigned long long)f->trap.tval);
  if (f->trap.has_paddr)
    n += snprintf(det + n, sizeof(det) - (size_t)n, " paddr=%016llx",
                  (unsigned long long)f->trap.paddr);
  snprintf(det + n, sizeof(det) - (size_t)n, " -> %016llx", (unsigned long long)FrameDnpc(f));
  TableRowCell(g_tbl, det);
  CellFlags(g_tbl, f);
  TableRowEnd(g_tbl);
  HistDump();
}

void DebugMem(frame* f, uint64_t addr, uint64_t watch_addr, int size, int acc,
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
      snprintf(det, sizeof(det), "%s %llx:%d %s %llx", is_load ? "ld" : "st",
               (unsigned long long)addr, size, is_load ? "->" : "=",
               (unsigned long long)(val_or_result & 0xffffffffULL));
      TableRowCell(g_tbl, det);
      CellFlags(g_tbl, f);
      TableRowEnd(g_tbl);
    }
  }
  if (DebugWatchHit(watch_addr, size, is_load) && Allow(kDbgWatch)) {
    if (mid_fetch)
      RowBeginBare(g_tbl, 'W');
    else
      RowBegin(g_tbl, 'W', f);
    // `inst=` orders several hits on the same address against each other: a
    // loader writes the code, something else clears it later, and the row that
    // decides the diagnosis is the one after the clear. Free when no watchpoint
    // fires -- this row only runs on a match.
    char det[96];
    snprintf(det, sizeof(det), "%c %llx:%d %s %llx inst=%llu", is_load ? 'r' : 'w',
             (unsigned long long)addr, size, is_load ? "->" : "<-",
             (unsigned long long)(val_or_result & 0xffffffffULL),
             (unsigned long long)f->cpu->inst_count);
    TableRowCell(g_tbl, det);
    CellFlags(g_tbl, f);
    TableRowEnd(g_tbl);
    HistDump();
  }
}

void DebugLinWatch(frame* f, uint64_t lin, int size, int is_load, uint64_t val) {
  if (!f || !DebugLinWatchHit(lin, size, is_load) || !Allow(kDbgWatch)) return;
  RowBegin(g_tbl, 'W', f);
  char det[96];
  snprintf(det, sizeof(det), "l%c %llx:%d %s %llx inst=%llu", is_load ? 'r' : 'w',
           (unsigned long long)lin, size, is_load ? "->" : "<-",
           (unsigned long long)(val & 0xffffffffULL),
           (unsigned long long)f->cpu->inst_count);
  TableRowCell(g_tbl, det);
  CellFlags(g_tbl, f);
  TableRowEnd(g_tbl);
  HistDump();
}

void DebugBus(frame* f, const char* dev_name, uint64_t addr, int size, int is_load, uint64_t val) {
  // The filter runs before the budget: a row the caller narrowed away must not
  // spend the window that the rows it asked for need.
  if (!DebugOn(kDbgBus) || !BusWanted(dev_name, addr) || !Allow(kDbgBus)) return;
  RowBegin(g_tbl, 'B', f);
  char det[64];
  snprintf(det, sizeof(det), "%s %s:%llx:%d %s %llx", is_load ? "io-ld" : "io-st",
           dev_name ? dev_name : "?", (unsigned long long)addr, size, is_load ? "->" : "=",
           (unsigned long long)(val & 0xffffffffULL));
  TableRowCell(g_tbl, det);
  CellFlags(g_tbl, f);
  TableRowEnd(g_tbl);
  HistDump();
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
  if (g_mark_name[0] && strcmp(g_mark_name, what) != 0) return;
  if (!DebugOn(kDbgMark) || !Allow(kDbgMark)) return;
  RowBeginBare(g_tbl, 'K');
  char det[112];
  snprintf(det, sizeof(det), "%s a=%d b=0x%x", what, a, (unsigned)b);
  AppendCtx(det, sizeof(det));
  TableRowCell(g_tbl, det);
  TableRowEnd(g_tbl);
  HistDump();
}

void DebugVirtio(const char* what, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  if (!DebugOn(kDbgVirtio) || !Allow(kDbgVirtio)) return;
  RowBeginBare(g_tbl, 'K');
  char det[144];
  snprintf(det, sizeof(det), "virtio %s a=%llx b=%llx c=%llx d=%llx", what,
           (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
           (unsigned long long)d);
  AppendCtx(det, sizeof(det));
  TableRowCell(g_tbl, det);
  TableRowEnd(g_tbl);
  HistDump();
}

void DebugPmpDeny(uint64_t addr, int len, int acc, int mode, int idx, unsigned cfg,
                  uint64_t base, uint64_t size) {
  if (!DebugOn(kDbgPmp) || !Allow(kDbgPmp)) return;
  static const char kAcc[] = "irwa";
  RowBeginBare(g_tbl, 'K');
  char det[144];
  if (idx < 0)
    snprintf(det, sizeof(det), "pmp-deny addr=%llx len=%d %c %c entry=none",
             (unsigned long long)addr, len, kAcc[acc & 3],
             mode == 3 ? 'M' : mode == 1 ? 'S' : 'U');
  else
    snprintf(det, sizeof(det),
             "pmp-deny addr=%llx len=%d %c %c entry=%d cfg=%02x base=%llx size=%llx",
             (unsigned long long)addr, len, kAcc[acc & 3],
             mode == 3 ? 'M' : mode == 1 ? 'S' : 'U', idx, cfg, (unsigned long long)base,
             (unsigned long long)size);
  AppendCtx(det, sizeof(det));
  TableRowCell(g_tbl, det);
  TableRowEnd(g_tbl);
  HistDump();
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
  // The vector the pairing return belongs to: gateret carries no vector of its
  // own (the guest returns through IRET), so the last delivered one is what
  // lets mark:gateret=N filter a single service's answers.
  g_last_gate_vec = (int)vec;
  // The function byte is what `mark:gatecall=VEC:AH` pairs the answer half
  // against; record it for every call so the gateret side can filter on the
  // request even when the request row itself was suppressed.
  g_last_gate_ah = (int)((a->arg_ax >> 8) & 0xff);
  if (g_mark_name[0] && strcmp(g_mark_name, "gatecall") != 0) return;
  if (g_mark_vec >= 0 && (int)vec != g_mark_vec) return;
  if (g_mark_ah >= 0 && (int)((a->arg_ax >> 8) & 0xff) != g_mark_ah) return;
  if (!DebugOn(kDbgMark) || !Allow(kDbgMark)) return;
  RowBeginBare(g_tbl, 'K');
  char det[128];
  snprintf(det, sizeof(det),
           "gatecall int=%02x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x ds=%04x es=%04x at=%04x:%04x",
           vec, a->arg_ax & 0xffff, a->arg_bx & 0xffff, a->arg_cx & 0xffff, a->arg_dx & 0xffff,
           a->arg_si & 0xffff, a->arg_ds & 0xffff, a->arg_es & 0xffff, (caller >> 16) & 0xffff,
           caller & 0xffff);
  TableRowCell(g_tbl, det);
  TableRowEnd(g_tbl);
}

// The answer half. BX/CX/DX are here because a handler returns through them too
// (int 10h AH=0Fh answers the column count in AH and the page in BH; DOS
// AH=45h answers with the duplicated handle in AX and AH=44h/0Dh uses BX); the
// caller's segments are not, since a handler that changed them changed the
// caller's context rather than answering anything.
void DebugGateRet(const DbgCallArgs* a, int cf, uint32_t caller) {
  if (g_mark_vec >= 0 && g_last_gate_vec != g_mark_vec) return;
  // Pair mode: `mark:gatecall=VEC:AH` prints both halves of that function, so the
  // answer (a DOS read's byte count and CF) is visible next to the request.
  int pair = g_mark_ah >= 0 && g_mark_name[0] && strcmp(g_mark_name, "gatecall") == 0;
  if (pair) {
    if (g_last_gate_ah != g_mark_ah) return;
  } else if (g_mark_name[0] && strcmp(g_mark_name, "gateret") != 0) {
    return;
  }
  if (!DebugOn(kDbgMark) || !Allow(kDbgMark)) return;
  RowBeginBare(g_tbl, 'K');
  char det[96];
  // The vector is the call's, carried over from the delivery: without it a
  // return row cannot say which multiplexer answered (and `cf=1` alone cannot
  // separate a DOS failure from a BIOS one).
  snprintf(det, sizeof(det), "gateret int=%02x ax=%04x bx=%04x cx=%04x dx=%04x cf=%d at=%04x:%04x",
           g_last_gate_vec, a->arg_ax & 0xffff, a->arg_bx & 0xffff, a->arg_cx & 0xffff,
           a->arg_dx & 0xffff, cf, (caller >> 16) & 0xffff, caller & 0xffff);
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
// A console line is the guest's own text, with nothing to put in the table's
// PC/RAW/MNEMONIC/FLAGS cells, so this category prints whole lines rather than
// a row of chrome around them.
//
// The buffer holds a whole console row, not just a sentence: the display's
// text grid is up to 132 cells and a CP437 glyph is three UTF-8 bytes, so 396
// bytes of text plus the kind and the newline. A shorter buffer would truncate
// exactly the full-screen rows this facility exists to read.
enum { kDebugTextMax = 512 };
void DebugText(const char* kind, const char* text) {
  // screenstop=TEXT: freeze the session on the console line that carries TEXT,
  // so dump=/regs at session end describe the moment the guest printed it
  // rather than whatever it wandered into afterwards (a fatal-error banner is
  // followed by a halt loop, and the state that matters is the one before it).
  // The hook rides the display's text publication — already a cold path.
  if (g_screen_stop[0] && text && strstr(text, g_screen_stop)) {
    if (g_frame) g_frame->cpu->halted = kCpuExited;
    g_screen_stop[0] = 0;  // one shot
  }
  if (!DebugOn(kDbgScreen) || !Allow(kDbgScreen)) return;
  char line[kDebugTextMax];
  int n = snprintf(line, sizeof(line), "%s %s\n", kind, text);
  if (n < 0) return;
  if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;  // truncated
  if (DebugAccountOut((size_t)n)) HostWriteErr(line, (size_t)n);
}

int DebugUtf8(void) { return !g_ascii; }

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
