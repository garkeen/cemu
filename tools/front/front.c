// cemugui — the cemu gdb-stub front-end (阶段 3.5 片 3). A pure RSP client:
// register / memory views, breakpoints, run-step-stop control. There is no
// disassembler here by decision (2026-09-12): instruction bytes are shown in
// the memory view and decoded with an external tool (llvm-objdump, or gdb /
// lldb attached to the same stub).
//
// Presentation: the simplest form that works — native controls with system
// colors (nothing custom-painted, so nothing can garble) and an ADAPTIVE
// layout: ComputeLayout() turns the client rectangle into every control
// rectangle plus the window's own minimum size, and Relayout() applies that as
// one atomic reflow (WS_CLIPCHILDREN + SWP_NOCOPYBITS + a single RedrawWindow
// over parent and children). Metrics scale with the monitor DPI, and
// WM_DPICHANGED adopts the size Windows hands over.
//
// Single-threaded, the same shape as the display window: a 100ms timer polls
// the socket while the guest runs (stop replies only arrive then); everything
// else is a request/response exchange while stopped.
#include <windows.h>
#include <commctrl.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsp.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0  // pre-1607 SDK headers
#endif

// ---- control ids -----------------------------------------------------------------
enum {
  IDC_HOST = 100,
  IDC_CONNECT,
  IDC_DETACH,
  IDC_RUN,
  IDC_STEP,
  IDC_STOP,
  IDC_STATUS,
  IDC_REGS,
  IDC_MEMADDR,
  IDC_GOTO,
  IDC_FOLLOW,
  IDC_MEM,
  IDC_BPADDR,
  IDC_BPADD,
  IDC_BPREMOVE,
  IDC_BPLIST,
};

// connection state (drives button enablement + status color)
enum { ST_DOWN = 0, ST_STOPPED, ST_RUNNING, ST_EXITED };

enum { kMemRows = 16, kMaxBps = 64 };  // 256 bytes per fetch fits PacketSize

// Light palette — VS Code "Light Modern" with GitHub-light status semantics.
static const COLORREF kColPanelBg = RGB(255, 255, 255);  // pane interiors
static const COLORREF kColText = RGB(59, 59, 59);          // #3B3B3B text

static Rsp* g_rsp;
static RspRegTable g_tab;
static int g_state = ST_DOWN;
static uint64_t g_mem_addr;
static uint64_t g_pc;
static uint64_t g_bps[kMaxBps];
static int g_nbps;

static HWND g_wnd, g_host, g_connect, g_detach, g_run, g_step, g_stop, g_status;
static HWND g_regs, g_memaddr, g_goto, g_follow, g_mem;
static HWND g_bplabel, g_bpaddr, g_bpadd, g_bpremove, g_bplist, g_hint;
static HWND g_cap_regs, g_cap_mem, g_cap_bp;  // pane captions (real controls:
                                              // parent-drawn chrome + partial
                                              // child repaints = garbage)
static HFONT g_ui, g_mono;
static UINT g_dpi = 96;

// ---- geometry slots ----------------------------------------------------------------
// One slot per control: ComputeLayout() writes a rect per slot and Relayout() applies
// them in one loop, so no control's rectangle is computed anywhere else.
enum {
  ctl_host, ctl_connect, ctl_detach, ctl_run, ctl_step, ctl_stop, ctl_status,
  ctl_cap_regs, ctl_cap_mem, ctl_cap_bp, ctl_regs,
  ctl_memaddr, ctl_goto, ctl_follow, ctl_mem,
  ctl_bplabel, ctl_bpaddr, ctl_bpadd, ctl_bpremove, ctl_bplist, ctl_hint,
  kCtlCount
};

static HWND* const g_ctl[kCtlCount] = {  // order must match the enum above
    &g_host,     &g_connect, &g_detach, &g_run,      &g_step,    &g_stop,     &g_status,
    &g_cap_regs, &g_cap_mem, &g_cap_bp, &g_regs,     &g_memaddr, &g_goto,     &g_follow,
    &g_mem,      &g_bplabel, &g_bpaddr, &g_bpadd,    &g_bpremove, &g_bplist,  &g_hint,
};

static void RefreshRegs(void);
static void RefreshMem(void);
static void ShowStopReply(const char* pkt);

// ---- DPI ------------------------------------------------------------------------------

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- helpers -----------------------------------------------------------------------

static void SetStatus(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  SetWindowTextA(g_status, buf);
}

static uint64_t EditHex(HWND edit) {
  char text[64];
  GetWindowTextA(edit, text, (int)sizeof(text));
  return strtoull(text, NULL, 16);
}

static void SetEditHex(HWND edit, uint64_t v) {
  char text[32];
  snprintf(text, sizeof(text), "%llx", (unsigned long long)v);
  SetWindowTextA(edit, text);
}

static void UpdateButtons(void) {
  EnableWindow(g_connect, g_state == ST_DOWN);
  EnableWindow(g_detach, g_state != ST_DOWN);
  EnableWindow(g_run, g_state == ST_STOPPED);
  EnableWindow(g_step, g_state == ST_STOPPED);
  EnableWindow(g_stop, g_state == ST_RUNNING);
  EnableWindow(g_memaddr, g_state == ST_STOPPED);
  EnableWindow(g_goto, g_state == ST_STOPPED);
  EnableWindow(g_follow, g_state == ST_STOPPED);
  EnableWindow(g_bpaddr, g_state == ST_STOPPED);
  EnableWindow(g_bpadd, g_state == ST_STOPPED);
  EnableWindow(g_bpremove, g_state == ST_STOPPED);
}

// The connection went away (or a request failed on it).
static void LoseConn(const char* why) {
  RspClose(g_rsp);
  g_rsp = NULL;
  g_state = ST_DOWN;
  UpdateButtons();
  SetStatus("%s", why);
}

static void ShowStopReply(const char* pkt) {
  if (pkt[0] == 'W' || pkt[0] == 'X') {
    g_state = ST_EXITED;
    UpdateButtons();
    SetStatus("guest exited (code %s)", pkt + 1);
    return;
  }
  g_state = ST_STOPPED;
  UpdateButtons();
  RefreshRegs();
  // Every stop re-centers the memory view on the stop address (gdb-like);
  // Goto can always point it elsewhere afterwards.
  g_mem_addr = g_pc;
  SetEditHex(g_memaddr, g_pc);
  RefreshMem();
  SetStatus("stopped (%s) - %d registers", pkt, g_tab.n);
}

// ---- views -------------------------------------------------------------------------

static void RefreshRegs(void) {
  char pkt[2048];
  if (RspSend(g_rsp, "g") || RspRecv(g_rsp, pkt, sizeof(pkt)) <= 0) {
    LoseConn("registers: connection lost");
    return;
  }
  ListView_DeleteAllItems(g_regs);
  const char* hex = pkt;
  g_pc = 0;
  int row = 0;
  for (int i = 0; i < g_tab.n; i++) {
    RspReg* reg = &g_tab.reg[i];
    uint8_t b[16];
    int got = 0;
    for (; got < reg->size && hex[0] && hex[1]; got++) {
      int hi = RspHexVal((uint8_t)hex[0]), lo = RspHexVal((uint8_t)hex[1]);
      if (hi < 0 || lo < 0) break;
      b[got] = (uint8_t)((hi << 4) | lo);
      hex += 2;
    }
    if (got < reg->size) break;  // short reply: show what we decoded
    if (reg->hidden) continue;
    char val[48];
    if (reg->size <= 8) {
      uint64_t v = 0;
      for (int k = got - 1; k >= 0; k--) v = (v << 8) | b[k];
      snprintf(val, sizeof(val), "0x%0*llx", got * 2, (unsigned long long)v);
      if (reg->code_ptr) g_pc = v;
    } else {
      // wide register (x87 80-bit): raw bytes, most significant first
      char* w = val;
      *w++ = '0';
      *w++ = 'x';
      for (int k = got - 1; k >= 0; k--) w += sprintf(w, "%02x", b[k]);
      *w = 0;
    }
    LVITEMA it;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.pszText = reg->name;
    ListView_InsertItem(g_regs, &it);
    ListView_SetItemText(g_regs, row, 1, val);
    row++;
  }
}

static void RefreshMem(void) {
  char cmd[48], pkt[1200];
  snprintf(cmd, sizeof(cmd), "m%llx,%x", (unsigned long long)g_mem_addr, kMemRows * 16);
  if (RspSend(g_rsp, cmd) || RspRecv(g_rsp, pkt, sizeof(pkt)) <= 0) {
    LoseConn("memory: connection lost");
    return;
  }
  char text[kMemRows * 90 + 64];
  int n = 0;
  if (pkt[0] == 'E') {
    snprintf(text, sizeof(text), "unmapped address\r\n");
  } else {
    for (int r = 0; r < kMemRows; r++) {
      n += snprintf(text + n, sizeof(text) - (size_t)n, "%016llx  ",
                    (unsigned long long)(g_mem_addr + (uint64_t)(r * 16)));
      char ascii[17];
      int have = 0;
      for (int c = 0; c < 16; c++) {
        int hi = RspHexVal((uint8_t)pkt[(r * 16 + c) * 2]);
        int lo = RspHexVal((uint8_t)pkt[(r * 16 + c) * 2 + 1]);
        if (hi < 0 || lo < 0) {  // short reply: pad the rest of the row
          n += snprintf(text + n, sizeof(text) - (size_t)n, "   ");
          ascii[c] = ' ';
          continue;
        }
        uint8_t v = (uint8_t)((hi << 4) | lo);
        n += snprintf(text + n, sizeof(text) - (size_t)n, "%02x ", v);
        ascii[c] = (v >= 0x20 && v < 0x7f) ? (char)v : '.';
        have = 1;
      }
      ascii[16] = 0;
      if (have) n += snprintf(text + n, sizeof(text) - (size_t)n, " %s", ascii);
      n += snprintf(text + n, sizeof(text) - (size_t)n, "\r\n");
    }
  }
  SetWindowTextA(g_mem, text);
}

// ---- actions -----------------------------------------------------------------------

static void DoConnect(void) {
  char hp[128];
  GetWindowTextA(g_host, hp, (int)sizeof(hp));
  g_rsp = RspConnect(hp);
  if (!g_rsp) {
    SetStatus("connect to %s failed (is cemu running with -s?)", hp);
    return;
  }
  char pkt[2048];
  // Handshake: announce (reply unused — the stub's feature set is fixed),
  // ask the stop reason (the stub stops a running guest on attach), then
  // learn the register layout from the target description.
  if (RspSend(g_rsp, "qSupported:PacketSize=1000") || RspRecv(g_rsp, pkt, sizeof(pkt)) < 0 ||
      RspSend(g_rsp, "?") || RspRecv(g_rsp, pkt, sizeof(pkt)) <= 0) {
    LoseConn("handshake failed");
    return;
  }
  if (RspReadRegTable(g_rsp, &g_tab)) {
    LoseConn("target.xml: no registers");
    return;
  }
  ShowStopReply(pkt);
}

static void DoDetach(void) {
  char pkt[64];
  if (g_state == ST_RUNNING) {  // the stub only serves packets while stopped
    RspInterrupt(g_rsp);
    if (RspRecv(g_rsp, pkt, sizeof(pkt)) <= 0) {
      LoseConn("connection lost");
      return;
    }
  }
  RspSend(g_rsp, "D");  // the guest keeps running after we let go
  RspClose(g_rsp);
  g_rsp = NULL;
  g_state = ST_DOWN;
  UpdateButtons();
  SetStatus("detached (guest runs free)");
}

static void DoRun(void) {
  if (RspSend(g_rsp, "c")) {
    LoseConn("connection lost");
    return;
  }
  g_state = ST_RUNNING;
  UpdateButtons();
  SetStatus("running - interrupt or wait for a breakpoint");
}

static void DoStep(void) {
  char pkt[64];
  if (RspSend(g_rsp, "s") || RspRecv(g_rsp, pkt, sizeof(pkt)) <= 0) {
    LoseConn("connection lost");
    return;
  }
  ShowStopReply(pkt);
}

static void DoBpAdd(void) {
  uint64_t addr = EditHex(g_bpaddr);
  if (!addr) return;
  for (int i = 0; i < g_nbps; i++)
    if (g_bps[i] == addr) return;
  char cmd[48], pkt[64];
  snprintf(cmd, sizeof(cmd), "Z0,%llx,1", (unsigned long long)addr);
  if (RspSend(g_rsp, cmd) || RspRecv(g_rsp, pkt, sizeof(pkt)) <= 0) {
    LoseConn("connection lost");
    return;
  }
  if (strcmp(pkt, "OK") != 0) {
    SetStatus("breakpoint refused (%s)", pkt);
    return;
  }
  g_bps[g_nbps++] = addr;
  char text[32];
  snprintf(text, sizeof(text), "0x%llx", (unsigned long long)addr);
  SendMessageA(g_bplist, LB_ADDSTRING, 0, (LPARAM)text);
  SetStatus("breakpoint at %llx", (unsigned long long)addr);
}

static void DoBpRemove(void) {
  int sel = (int)SendMessageA(g_bplist, LB_GETCURSEL, 0, 0);
  if (sel < 0 || sel >= g_nbps) return;
  char cmd[48], pkt[64];
  snprintf(cmd, sizeof(cmd), "z0,%llx,1", (unsigned long long)g_bps[sel]);
  if (RspSend(g_rsp, cmd) || RspRecv(g_rsp, pkt, sizeof(pkt)) <= 0) {
    LoseConn("connection lost");
    return;
  }
  for (int i = sel; i < g_nbps - 1; i++) g_bps[i] = g_bps[i + 1];
  g_nbps--;
  SendMessageA(g_bplist, LB_DELETESTRING, (WPARAM)sel, 0);
  SetStatus("breakpoint removed");
}

// ---- adaptive layout ----------------------------------------------------------------
// ComputeLayout() is the single source of every control rectangle: the client size goes
// in, all kCtlCount rects come out, plus the smallest client size this arrangement can
// be applied to. Relayout() only applies the result, so geometry can never disagree with
// itself — the old split (three pane rects here, hardcoded pixel advances there) is what
// let two controls end up drawn over each other. Zone sizes are proportional with
// clamps, so any size at or above the minimum, at any DPI, produces a valid arrangement.

enum {
  kMargin = 10,
  kGap = 8,
  kRowH = 26,         // edit boxes
  kBtnH = 28,         // buttons
  kCapH = 18,         // pane captions
  kLblH = 20,         // small labels
  kRegsMinW = 260,    // registers column: floor and ceiling
  kRegsMaxW = 430,
  kBpMinW = 430,      // breakpoints pane: floor and ceiling
  kBpMaxW = 700,
  kBottomMinH = 120,  // bottom strip: floor and ceiling
  kBottomMaxH = 220,
  kMemStripW = 344,   // addr edit + Goto + Follow PC + margins (memory pane's own floor)
  kHintMinW = 200,
  kPaneMinH = 206,    // caption band + a usable register/memory list
  kStatusMinW = 60,
  kRegsFracPct = 30,  // share of the width before clamping
  kBpFracPct = 48,
  kBottomFracPct = 24,
  kInitW = 1160,      // initial window size, scaled by S() at creation
  kInitH = 720,
};

typedef struct {
  int need_cw, need_ch;  // smallest client rect this layout can be applied to
} Layout;

static void SlotRect(RECT* r, int x, int y, int w, int h) {
  r->left = x;
  r->top = y;
  r->right = x + (w > 0 ? w : 1);
  r->bottom = y + (h > 0 ? h : 1);
}

// Toolbar: one left-to-right flow; returns the x where the status label starts.
static int ToolbarFlow(RECT out[kCtlCount]) {
  static const int kBox[][3] = {  // ctl, width, height (96-dpi units)
      {ctl_host, 150, kRowH}, {ctl_connect, 80, kBtnH}, {ctl_detach, 80, kBtnH},
      {ctl_run, 84, kBtnH},   {ctl_step, 84, kBtnH},    {ctl_stop, 96, kBtnH},
  };
  int x = S(kMargin);
  for (size_t i = 0; i < sizeof(kBox) / sizeof(kBox[0]); i++) {
    int w = S(kBox[i][1]);
    if (out) SlotRect(&out[kBox[i][0]], x, S(kMargin), w, S(kBox[i][2]));
    x += w + S(kGap);
  }
  return x;
}

static void ComputeLayout(int cw, int ch, RECT out[kCtlCount], Layout* meta) {
  const int m = S(kMargin), gap = S(kGap);
  const int pane_y = m + S(kBtnH) + S(10);

  // The layout states its own minimum (no separate magic minimum size): toolbar plus a
  // usable status label, the widest fixed control strip of each pane, and the bottom
  // strip's floor.
  int tb_min = ToolbarFlow(NULL) + S(kStatusMinW) + m;
  int mem_min = m + S(kRegsMinW) + gap + S(kMemStripW) + m;
  int bp_min = m + S(kBpMinW) + gap + S(kHintMinW) + m;
  meta->need_cw = tb_min > mem_min ? (tb_min > bp_min ? tb_min : bp_min)
                                   : (mem_min > bp_min ? mem_min : bp_min);
  meta->need_ch = pane_y + S(kPaneMinH) + S(kBottomMinH) + m;
  if (!out) return;

  int x = ToolbarFlow(out);
  SlotRect(&out[ctl_status], x, m + S(5),
           cw - x - m > S(kStatusMinW) ? cw - x - m : S(kStatusMinW), S(kLblH));

  int bottom_h = Clamp((ch - pane_y) * kBottomFracPct / 100, S(kBottomMinH), S(kBottomMaxH));
  int pane_h = ch - pane_y - bottom_h - m;
  int left_w = Clamp((cw - 2 * m - gap) * kRegsFracPct / 100, S(kRegsMinW), S(kRegsMaxW));
  RECT regs, mem, bp;
  SlotRect(&regs, m, pane_y, left_w, pane_h);
  SlotRect(&mem, m + left_w + gap, pane_y, cw - m - (m + left_w + gap), pane_h);
  int by = ch - bottom_h;
  int bp_w = Clamp((cw - 2 * m - gap) * kBpFracPct / 100, S(kBpMinW), S(kBpMaxW));
  SlotRect(&bp, m, by, bp_w, bottom_h - m);

  // pane interiors; the captions sit in each pane's top band
  int regs_w = regs.right - regs.left, regs_h = regs.bottom - regs.top;
  SlotRect(&out[ctl_cap_regs], regs.left + S(10), regs.top + S(4), regs_w - S(20), S(kCapH));
  SlotRect(&out[ctl_regs], regs.left + S(10), regs.top + S(26), regs_w - S(20), regs_h - S(36));

  int mem_x = mem.left, mem_w = mem.right - mem.left, mem_h = mem.bottom - mem.top;
  SlotRect(&out[ctl_cap_mem], mem_x + S(10), mem.top + S(4), mem_w - S(20), S(kCapH));
  SlotRect(&out[ctl_memaddr], mem_x + S(10), mem.top + S(24), S(160), S(kRowH));
  SlotRect(&out[ctl_goto], mem_x + S(178), mem.top + S(23), S(56), S(kBtnH));
  SlotRect(&out[ctl_follow], mem_x + S(242), mem.top + S(23), S(92), S(kBtnH));
  SlotRect(&out[ctl_mem], mem_x + S(10), mem.top + S(58), mem_w - S(20), mem_h - S(68));

  int bp_wide = bp.right - bp.left;
  SlotRect(&out[ctl_cap_bp], bp.left + S(10), bp.top + S(4), bp_wide - S(20), S(kCapH));
  SlotRect(&out[ctl_bplabel], bp.left + S(10), by + S(28), S(40), S(kLblH));
  SlotRect(&out[ctl_bpaddr], bp.left + S(56), by + S(24), S(140), S(kRowH));
  SlotRect(&out[ctl_bpadd], bp.left + S(204), by + S(23), S(80), S(kBtnH));
  SlotRect(&out[ctl_bpremove], bp.left + S(292), by + S(23), S(100), S(kBtnH));
  SlotRect(&out[ctl_bplist], bp.left + S(10), by + S(58), bp_wide - S(20),
           bp.bottom - by - S(66));

  // The hint starts right of BOTH bottom-row panes: the breakpoints pane can be wider
  // than the registers column, so anchoring it to the memory pane's left edge let the
  // two controls overlap and double-paint.
  int hint_x = (bp.right > mem.left ? bp.right : mem.left) + gap;
  SlotRect(&out[ctl_hint], hint_x, by + S(12), cw - hint_x - m, S(56));
}

static void Relayout(void) {
  if (!g_wnd) return;
  RECT rc;
  GetClientRect(g_wnd, &rc);
  RECT r[kCtlCount];
  Layout meta;
  ComputeLayout(rc.right, rc.bottom, r, &meta);

  // The register columns follow the pane (fixed column widths went stale on DPI change)
  if (g_regs) {
    int w = r[ctl_regs].right - r[ctl_regs].left;
    int c0 = w * 45 / 100;
    ListView_SetColumnWidth(g_regs, 0, c0);
    ListView_SetColumnWidth(g_regs, 1, w - c0 - S(4));
  }

  // One atomic reflow: the batch carries SWP_NOCOPYBITS so the system never shifts stale
  // pixels around, redraw is frozen across the batch, and a single RedrawWindow restores
  // parent and children afterwards. A bare SetWindowPos per control — no clip, no
  // no-copy-bits, no erase — is exactly what smeared shadows around the window.
  SendMessageA(g_wnd, WM_SETREDRAW, FALSE, 0);
  HDWP h = BeginDeferWindowPos(kCtlCount);
  for (int i = 0; i < kCtlCount && h; i++) {
    HWND c = *g_ctl[i];
    if (!c) continue;
    h = DeferWindowPos(h, c, NULL, r[i].left, r[i].top, r[i].right - r[i].left,
                       r[i].bottom - r[i].top,
                       SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
  }
  if (h) EndDeferWindowPos(h);
  SendMessageA(g_wnd, WM_SETREDRAW, TRUE, 0);
  RedrawWindow(g_wnd, NULL, NULL,
               RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

// ---- window plumbing ---------------------------------------------------------------

// WS_CLIPCHILDREN is the first half of the reflow contract: the parent's background
// erase must never touch a child's pixels (that is what left stale patches behind).
// The second half lives in Relayout(): the deferred batch carries SWP_NOCOPYBITS and
// the reflow ends with one RedrawWindow covering parent and children.
static const DWORD kWindowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;

// Controls are created at (0,0,0,0) with no geometry of their own; Relayout() is the
// only thing that ever gives them a rectangle.
static HWND MakeEdit(int id, DWORD style) {
  HWND h = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | style, 0, 0, 0, 0,
                         g_wnd, (HMENU)(INT_PTR)id, NULL, NULL);
  SendMessageA(h, WM_SETFONT, (WPARAM)g_mono, TRUE);
  return h;
}

static HWND MakeLabelStyle(const char* text, DWORD style) {
  HWND h = CreateWindowA("STATIC", text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g_wnd,
                         NULL, NULL, NULL);
  SendMessageA(h, WM_SETFONT, (WPARAM)g_ui, TRUE);
  return h;
}

static HWND MakeLabel(const char* text) { return MakeLabelStyle(text, 0); }

static HWND MakeBtn(const char* text, int id) {
  HWND h = CreateWindowA("BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                         0, 0, 0, 0, g_wnd, (HMENU)(INT_PTR)id, NULL, NULL);
  SendMessageA(h, WM_SETFONT, (WPARAM)g_ui, TRUE);
  return h;
}

static void MakeFonts(void) {
  g_ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  g_mono = CreateFontA(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
                       "Consolas");
}

static void ApplyDpi(HWND wnd) {
  UINT (WINAPI *pGetDpi)(HWND) =
      (UINT (WINAPI*)(HWND))GetProcAddress(GetModuleHandleA("user32.dll"),
                                           "GetDpiForWindow");
  g_dpi = pGetDpi ? pGetDpi(wnd) : 96;
  if (g_dpi < 96) g_dpi = 96;
  if (g_mono) DeleteObject(g_mono);
  MakeFonts();
  HWND mono_ctrls[] = {g_regs, g_memaddr, g_mem, g_bpaddr, g_bplist};
  for (size_t i = 0; i < sizeof(mono_ctrls) / sizeof(mono_ctrls[0]); i++)
    SendMessageA(mono_ctrls[i], WM_SETFONT, (WPARAM)g_mono, TRUE);
  Relayout();
}

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      g_wnd = wnd;
      MakeFonts();
      g_host = MakeEdit(IDC_HOST, ES_AUTOHSCROLL);
      SetWindowTextA(g_host, "127.0.0.1:1234");
      g_connect = MakeBtn("Connect", IDC_CONNECT);
      g_detach = MakeBtn("Detach", IDC_DETACH);
      g_run = MakeBtn("Run (F5)", IDC_RUN);
      g_step = MakeBtn("Step (F10)", IDC_STEP);
      g_stop = MakeBtn("Interrupt", IDC_STOP);
      g_cap_regs = MakeLabelStyle("REGISTERS", 0);
      g_cap_mem = MakeLabelStyle("MEMORY", 0);
      g_cap_bp = MakeLabelStyle("BREAKPOINTS", 0);
      g_status = MakeLabelStyle("disconnected", SS_RIGHT);
      g_regs = CreateWindowExA(0, WC_LISTVIEWA, "",
                               WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT |
                                   LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                               0, 0, 0, 0, wnd, (HMENU)(INT_PTR)IDC_REGS, NULL, NULL);
      SendMessageA(g_regs, WM_SETFONT, (WPARAM)g_mono, TRUE);
      ListView_SetExtendedListViewStyle(g_regs, LVS_EX_FULLROWSELECT);
      ListView_SetBkColor(g_regs, kColPanelBg);
      ListView_SetTextBkColor(g_regs, kColPanelBg);
      ListView_SetTextColor(g_regs, kColText);
      // The column widths are derived from the pane rect on every Relayout(), so they
      // follow the pane and the DPI instead of being frozen at creation time.
      LVCOLUMNA col;
      memset(&col, 0, sizeof(col));
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = "register";
      ListView_InsertColumn(g_regs, 0, &col);
      col.pszText = "value";
      ListView_InsertColumn(g_regs, 1, &col);
      g_memaddr = MakeEdit(IDC_MEMADDR, ES_AUTOHSCROLL);
      g_goto = MakeBtn("Goto", IDC_GOTO);
      g_follow = MakeBtn("Follow PC", IDC_FOLLOW);
      g_mem = MakeEdit(IDC_MEM, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);
      g_bplabel = MakeLabel("addr:");
      g_bpaddr = MakeEdit(IDC_BPADDR, ES_AUTOHSCROLL);
      g_bpadd = MakeBtn("Add (Z0)", IDC_BPADD);
      g_bpremove = MakeBtn("Remove (z0)", IDC_BPREMOVE);
      g_bplist = CreateWindowA("LISTBOX", "",
                               WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                                   LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                               0, 0, 0, 0, wnd, (HMENU)(INT_PTR)IDC_BPLIST, NULL, NULL);
      SendMessageA(g_bplist, WM_SETFONT, (WPARAM)g_mono, TRUE);
      g_hint = MakeLabelStyle("no disassembler by design - decode with external\r\n"
                              "tools (llvm-objdump, or gdb on the same stub)",
                              0);
      SetTimer(wnd, 1, 100, NULL);
      ApplyDpi(wnd);
      UpdateButtons();
      return 0;
    }
    case WM_SIZE:
      Relayout();
      return 0;
    case WM_DPICHANGED: {
      // Windows hands over the size this window must have at the new DPI; ignoring it
      // left the old physical size while S() scaled every metric (controls overflowed).
      const RECT* sug = (const RECT*)lp;
      SetWindowPos(wnd, NULL, sug->left, sug->top, sug->right - sug->left,
                   sug->bottom - sug->top, SWP_NOZORDER | SWP_NOACTIVATE);
      ApplyDpi(wnd);  // rebuild the fonts for the new DPI, then Relayout()
      return 0;
    }
    case WM_TIMER:
      if (g_state == ST_RUNNING && g_rsp) {
        char pkt[64];
        int rc = RspPoll(g_rsp, pkt, sizeof(pkt));
        if (rc < 0)
          LoseConn("connection lost");
        else if (rc > 0)
          ShowStopReply(pkt);
      }
      return 0;
    case WM_COMMAND: {
      int id = LOWORD(wp);
      if (id == IDC_CONNECT && g_state == ST_DOWN)
        DoConnect();
      else if (id == IDC_DETACH && g_rsp)
        DoDetach();
      else if (id == IDC_RUN && g_state == ST_STOPPED)
        DoRun();
      else if (id == IDC_STEP && g_state == ST_STOPPED)
        DoStep();
      else if (id == IDC_STOP && g_state == ST_RUNNING)
        RspInterrupt(g_rsp);
      else if (id == IDC_GOTO && g_state == ST_STOPPED) {
        g_mem_addr = EditHex(g_memaddr);
        RefreshMem();
      } else if (id == IDC_FOLLOW && g_state == ST_STOPPED) {
        SetEditHex(g_memaddr, g_pc);
        g_mem_addr = g_pc;
        RefreshMem();
      } else if (id == IDC_BPADD && g_state == ST_STOPPED)
        DoBpAdd();
      else if (id == IDC_BPREMOVE && g_state == ST_STOPPED)
        DoBpRemove();
      return 0;
    }
    case WM_GETMINMAXINFO: {
      // The layout states its own minimum client size (no second magic number), and
      // ptMinTrackSize counts the frame, so fold it in.
      MINMAXINFO* mmi = (MINMAXINFO*)lp;
      Layout meta;
      ComputeLayout(0, 0, NULL, &meta);
      RECT rc = {0, 0, meta.need_cw, meta.need_ch};
      AdjustWindowRect(&rc, kWindowStyle, FALSE);
      mmi->ptMinTrackSize.x = rc.right - rc.left;
      mmi->ptMinTrackSize.y = rc.bottom - rc.top;
      return 0;
    }
    case WM_DESTROY:
      KillTimer(wnd, 1);
      if (g_rsp) {
        RspSend(g_rsp, "D");  // let the guest run free on the way out
        RspClose(g_rsp);
        g_rsp = NULL;
      }
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcA(wnd, msg, wp, lp);
  }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
  (void)prev;
  (void)cmd;
  // crisp rendering at any scale factor; silently classic on old systems
  BOOL (WINAPI *pSetCtx)(void*) =
      (BOOL (WINAPI*)(void*))GetProcAddress(GetModuleHandleA("user32.dll"),
                                            "SetProcessDpiAwarenessContext");
  if (pSetCtx) pSetCtx((void*)-4);  // PER_MONITOR_AWARE_V2
  // the initial window size must come out DPI-scaled, so learn the primary
  // monitor's factor before CreateWindow (per-window value confirmed later)
  HDC screen = GetDC(NULL);
  g_dpi = (UINT)GetDeviceCaps(screen, LOGPIXELSX);
  if (g_dpi < 96) g_dpi = 96;
  ReleaseDC(NULL, screen);

  INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES};
  InitCommonControlsEx(&icc);

  WNDCLASSA wc;
  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = "cemugui";
  RegisterClassA(&wc);

  // F5 = run, F10 = step (accelerators reach the buttons even when focus
  // sits in an edit box)
  ACCEL acc[2] = {{FVIRTKEY, VK_F5, IDC_RUN}, {FVIRTKEY, VK_F10, IDC_STEP}};
  HACCEL acc_tab = CreateAcceleratorTableA(acc, 2);

  HWND wnd = CreateWindowA(
      "cemugui", "cemugui - cemu debug front (no disassembler: use llvm-objdump / gdb)",
      kWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT, S(kInitW), S(kInitH), NULL, NULL, inst,
      NULL);
  (void)wnd;
  ShowWindow(wnd, show);
  MSG msg;
  while (GetMessageA(&msg, NULL, 0, 0) > 0) {
    if (!TranslateAcceleratorA(wnd, acc_tab, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
  }
  DestroyAcceleratorTable(acc_tab);
  return (int)msg.wParam;
}
