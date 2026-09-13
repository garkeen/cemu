// cemugui — the cemu gdb-stub front-end (阶段 3.5 片 3). A pure RSP client:
// register / memory views, breakpoints, run-step-stop control. There is no
// disassembler here by decision (2026-09-12): instruction bytes are shown in
// the memory view and decoded with an external tool (llvm-objdump, or gdb /
// lldb attached to the same stub).
//
// Presentation: the simplest form that works — native controls with system
// colors (nothing custom-painted, so nothing can garble) and an ADAPTIVE
// layout: every zone is derived from the client rectangle by one
// ComputeLayout() that Relayout uses; control sizes scale with the monitor
// DPI and the initial window size scales with it too.
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
  // owner-drawn buttons repaint only on demand
  HWND btns[] = {g_connect, g_detach, g_run, g_step, g_stop, g_goto, g_follow,
                 g_bpadd, g_bpremove};
  for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) InvalidateRect(btns[i], NULL, FALSE);
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
// One ComputeLayout() derives every zone from the client rectangle; Relayout
// positions the controls inside those zones and the pane painter frames the
// same rects — the two can never disagree. All zone sizes are proportional
// with sane clamps, so any window size and any DPI scale produce a valid
// arrangement.

typedef struct {
  RECT regs, mem, bp;  // pane frames (captions live in the top band)
  int row_y;           // toolbar baseline
  int ctl_h, btn_h;
} Layout;

static void ComputeLayout(int cw, int ch, Layout* L) {
  const int m = S(10), gap = S(8);
  L->ctl_h = S(26);
  L->btn_h = S(28);
  L->row_y = m;

  int pane_y = m + L->btn_h + S(10);
  int bottom_h = Clamp((ch - pane_y) * 24 / 100, S(120), S(220));
  int pane_h = ch - pane_y - bottom_h - m;
  int left_w = Clamp((cw - 2 * m - gap) * 30 / 100, S(260), S(430));
  int mem_x = m + left_w + gap;
  int by = ch - bottom_h;
  int bp_w = Clamp((cw - 2 * m - gap) * 48 / 100, S(430), S(700));

  L->regs = (RECT){m, pane_y, m + left_w, pane_y + pane_h};
  L->mem = (RECT){mem_x, pane_y, cw - m, pane_y + pane_h};
  L->bp = (RECT){m, by, m + bp_w, by + bottom_h - m};
}

static void Place(HWND h, int x, int y, int w, int h_) {
  SetWindowPos(h, NULL, x, y, w, h_, SWP_NOZORDER);
}

static void Relayout(void) {
  RECT rc;
  GetClientRect(g_wnd, &rc);
  int cw = rc.right, ch = rc.bottom;
  Layout L;
  ComputeLayout(cw, ch, &L);
  const int m = S(10), gap = S(8);
  const int mem_x = L.mem.left;
  const int by = L.bp.top;

  // toolbar: flow left to right, then give the status label the remainder
  int x = m;
  Place(g_host, x, L.row_y, S(150), L.ctl_h); x += S(158);
  Place(g_connect, x, L.row_y, S(80), L.btn_h); x += S(88);
  Place(g_detach, x, L.row_y, S(80), L.btn_h); x += S(88) + gap;
  Place(g_run, x, L.row_y, S(84), L.btn_h); x += S(92);
  Place(g_step, x, L.row_y, S(84), L.btn_h); x += S(92);
  Place(g_stop, x, L.row_y, S(96), L.btn_h); x += S(104);
  Place(g_status, x, L.row_y + S(5), cw - x - m > S(60) ? cw - x - m : S(60), S(20));

  // pane captions sit in each pane's top band
  Place(g_cap_regs, L.regs.left + S(10), L.regs.top + S(4), S(200), S(18));
  Place(g_cap_mem, L.mem.left + S(10), L.mem.top + S(4), S(200), S(18));
  Place(g_cap_bp, L.bp.left + S(10), L.bp.top + S(4), S(200), S(18));

  // registers pane
  Place(g_regs, L.regs.left + S(10), L.regs.top + S(26), L.regs.right - L.regs.left - S(20),
        L.regs.bottom - L.regs.top - S(36));

  // memory pane
  Place(g_memaddr, mem_x + S(10), L.mem.top + S(24), S(160), L.ctl_h);
  Place(g_goto, mem_x + S(178), L.mem.top + S(23), S(56), L.btn_h);
  Place(g_follow, mem_x + S(242), L.mem.top + S(23), S(92), L.btn_h);
  Place(g_mem, mem_x + S(10), L.mem.top + S(58), L.mem.right - mem_x - S(20),
        L.mem.bottom - L.mem.top - S(68));

  // breakpoints strip + hint
  Place(g_bplabel, L.bp.left + S(10), by + S(28), S(40), S(20));
  Place(g_bpaddr, L.bp.left + S(56), by + S(24), S(140), L.ctl_h);
  Place(g_bpadd, L.bp.left + S(204), by + S(23), S(80), L.btn_h);
  Place(g_bpremove, L.bp.left + S(292), by + S(23), S(100), L.btn_h);
  Place(g_bplist, L.bp.left + S(10), by + S(58), L.bp.right - L.bp.left - S(20),
        L.bp.bottom - by - S(66));
  // the hint starts right of BOTH bottom-row panes: the breakpoints pane
  // can be wider than the registers column, so anchoring the hint to the
  // memory pane's left edge let the two controls overlap and double-paint.
  int hint_x = (L.bp.right > L.mem.left ? L.bp.right : L.mem.left) + gap;
  Place(g_hint, hint_x, by + S(12), cw - hint_x - m, S(56));
}

// ---- window plumbing ---------------------------------------------------------------

static HWND MakeEdit(int id, int x, int y, int w, DWORD style) {
  HWND h = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | style, x, y, w, S(26),
                         g_wnd, (HMENU)(INT_PTR)id, NULL, NULL);
  SendMessageA(h, WM_SETFONT, (WPARAM)g_mono, TRUE);
  return h;
}

static HWND MakeLabelStyle(const char* text, int x, int y, int w, DWORD style) {
  HWND h = CreateWindowA("STATIC", text, WS_CHILD | WS_VISIBLE | style, x, y, w, S(20), g_wnd,
                         NULL, NULL, NULL);
  SendMessageA(h, WM_SETFONT, (WPARAM)g_ui, TRUE);
  return h;
}

static HWND MakeLabel(const char* text, int x, int y, int w) {
  return MakeLabelStyle(text, x, y, w, 0);
}

static HWND MakeBtnBase(const char* text, int id) {
  HWND h = CreateWindowA("BUTTON", text,
                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0, g_wnd,
                         (HMENU)(INT_PTR)id, NULL, NULL);
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
      g_host = MakeEdit(IDC_HOST, 0, 0, 0, ES_AUTOHSCROLL);
      SetWindowTextA(g_host, "127.0.0.1:1234");
      g_connect = MakeBtnBase("Connect", IDC_CONNECT);
      g_detach = MakeBtnBase("Detach", IDC_DETACH);
      g_run = MakeBtnBase("Run (F5)", IDC_RUN);
      g_step = MakeBtnBase("Step (F10)", IDC_STEP);
      g_stop = MakeBtnBase("Interrupt", IDC_STOP);
      g_cap_regs = MakeLabelStyle("REGISTERS", 0, 0, 0, 0);
      g_cap_mem = MakeLabelStyle("MEMORY", 0, 0, 0, 0);
      g_cap_bp = MakeLabelStyle("BREAKPOINTS", 0, 0, 0, 0);
      g_status = MakeLabelStyle("disconnected", 0, 0, 0, SS_RIGHT);
      g_regs = CreateWindowExA(0, WC_LISTVIEWA, "",
                               WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT |
                                   LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                               0, 0, 0, 0, wnd, (HMENU)(INT_PTR)IDC_REGS, NULL, NULL);
      SendMessageA(g_regs, WM_SETFONT, (WPARAM)g_mono, TRUE);
      ListView_SetExtendedListViewStyle(g_regs, LVS_EX_FULLROWSELECT);
      ListView_SetBkColor(g_regs, kColPanelBg);
      ListView_SetTextBkColor(g_regs, kColPanelBg);
      ListView_SetTextColor(g_regs, kColText);
      LVCOLUMNA col;
      memset(&col, 0, sizeof(col));
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = "register";
      col.cx = S(120);
      ListView_InsertColumn(g_regs, 0, &col);
      col.pszText = "value";
      col.cx = S(150);
      ListView_InsertColumn(g_regs, 1, &col);
      g_memaddr = MakeEdit(IDC_MEMADDR, 0, 0, 0, ES_AUTOHSCROLL);
      g_goto = MakeBtnBase("Goto", IDC_GOTO);
      g_follow = MakeBtnBase("Follow PC", IDC_FOLLOW);
      g_mem = MakeEdit(IDC_MEM, 0, 0, 0,
                       ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);
      g_bplabel = MakeLabel("addr:", 0, 0, 0);
      g_bpaddr = MakeEdit(IDC_BPADDR, 0, 0, 0, ES_AUTOHSCROLL);
      g_bpadd = MakeBtnBase("Add (Z0)", IDC_BPADD);
      g_bpremove = MakeBtnBase("Remove (z0)", IDC_BPREMOVE);
      g_bplist = CreateWindowA("LISTBOX", "",
                               WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                                   LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                               0, 0, 0, 0, wnd, (HMENU)(INT_PTR)IDC_BPLIST, NULL, NULL);
      SendMessageA(g_bplist, WM_SETFONT, (WPARAM)g_mono, TRUE);
      g_hint = MakeLabelStyle("no disassembler by design - decode with external\r\n"
                              "tools (llvm-objdump, or gdb on the same stub)", 0, 0, 0, 0);
      SetTimer(wnd, 1, 100, NULL);
      ApplyDpi(wnd);
      UpdateButtons();
      return 0;
    }
    case WM_SIZE:
      Relayout();
      return 0;
    case WM_DPICHANGED:
      ApplyDpi(wnd);
      return 0;
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
      MINMAXINFO* mmi = (MINMAXINFO*)lp;
      mmi->ptMinTrackSize.x = S(900);
      mmi->ptMinTrackSize.y = S(560);
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
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(1160), S(720), NULL, NULL, inst,
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
