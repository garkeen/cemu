// cemugui — the cemu gdb-stub front-end (阶段 3.5 片 3). A pure RSP client:
// register / memory views, breakpoints, run-step-stop control. There is no
// disassembler here by decision (2026-09-12): instruction bytes are shown in
// the memory view and decoded with an external tool (llvm-objdump, or gdb /
// lldb attached to the same stub).
//
// Presentation: dark Win10/11 theme built only from system APIs — the DWM
// immersive-dark titlebar attribute, uxtheme's DarkMode_Explorer control
// theme (the same one Explorer uses), owner-drawn flat buttons with hover
// tracking, per-monitor-V2 DPI awareness and a scaled layout. Every API here
// ships with Windows; there are no third-party dependencies.
//
// Single-threaded, the same shape as the display window: a 100ms timer
// polls the socket while the guest runs (stop replies only arrive then);
// everything else is a request/response exchange while stopped.
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsp.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0  // pre-1607 SDK headers
#endif

// GetProcAddress is untyped by nature; the strict prototype-cast warning is
// suppressed only around these resolutions.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wincompatible-function-pointer-types"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0  // pre-1607 SDK headers
#endif

// GetProcAddress is untyped by nature; the strict prototype-cast warning is
// suppressed only around these resolutions.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wincompatible-function-pointer-types"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20  // Win10 1809+ (19 on early builds)
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33  // Win11: round the corners
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

// dark palette (Win11-style grays + the system accent blue)
static const COLORREF kColBg = RGB(32, 32, 32);          // window background
static const COLORREF kColPanelLine = RGB(58, 58, 58);   // pane borders
static const COLORREF kColCaption = RGB(148, 148, 148);  // pane captions
static const COLORREF kColEditBg = RGB(37, 37, 37);      // edits / list body
static const COLORREF kColEditText = RGB(212, 212, 212);
static const COLORREF kColMemText = RGB(186, 186, 186);
static const COLORREF kColBtnBg = RGB(45, 45, 45);
static const COLORREF kColBtnHover = RGB(58, 58, 58);
static const COLORREF kColBtnPress = RGB(28, 28, 28);
static const COLORREF kColBtnLine = RGB(76, 76, 76);
static const COLORREF kColBtnText = RGB(222, 222, 222);
static const COLORREF kColAccent = RGB(0, 120, 212);

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
static HFONT g_ui, g_mono, g_caption;
static UINT g_dpi = 96;
static COLORREF g_status_col = RGB(160, 160, 160);

static void RefreshRegs(void);
static void RefreshMem(void);
static void ShowStopReply(const char* pkt);

// ---- dark-mode plumbing -------------------------------------------------------------

// uxtheme's dark-mode helpers are ordinal-only exports (the same entry
// points Explorer/Notepad use); resolved dynamically so pre-1809 systems
// simply fall back to the classic light theme.
static HRESULT (WINAPI *pAllowDark)(HWND, BOOL);
static void (WINAPI *pFlushThemes)(void);
static void (WINAPI *pSetAppMode)(int);  // PreferredAppMode: 1 = AllowDark
// The bare SetWindowTheme is a UNICODE-mapped macro and mingw's import lib
// carries only the mapped name; resolve the W entry like the ordinals.
static HRESULT (WINAPI *pSetTheme)(HWND, const wchar_t*, const wchar_t*);

static void DarkModeInit(void) {
  HMODULE ux = GetModuleHandleA("uxtheme.dll");
  if (ux) {
    pAllowDark = (void (WINAPI*)(HWND, BOOL))GetProcAddress(ux, MAKEINTRESOURCEA(133));
    pFlushThemes = (void (WINAPI*)(void))GetProcAddress(ux, MAKEINTRESOURCEA(136));
    pSetAppMode = (void (WINAPI*)(int))GetProcAddress(ux, MAKEINTRESOURCEA(137));
    pSetTheme = (HRESULT (WINAPI*)(HWND, const wchar_t*, const wchar_t*))(void*)GetProcAddress(
        ux, "SetWindowThemeW");
    if (pSetAppMode) pSetAppMode(1);  // AllowDark: dark when the app opts in
    if (pFlushThemes) pFlushThemes();
  }
}

#pragma GCC diagnostic pop

#pragma GCC diagnostic pop

static void DarkModeWindow(HWND wnd) {
  BOOL on = TRUE;
  // attribute 20 on current builds; 19 on the earliest 1809, so try both —
  // a failure just leaves the titlebar light.
  if (DwmSetWindowAttribute(wnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &on, sizeof(on)) != S_OK)
    DwmSetWindowAttribute(wnd, DWMWA_USE_IMMERSIVE_DARK_MODE - 1, &on, sizeof(on));
  DWORD pref = 2;  // DWMWCP_ROUND (Win11 rounded corners; ignored before)
  DwmSetWindowAttribute(wnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
  if (pAllowDark) pAllowDark(wnd, TRUE);
  if (pSetTheme) pSetTheme(wnd, L"DarkMode_Explorer", NULL);
}

static void DarkModeChild(HWND h) {
  if (pAllowDark) pAllowDark(h, TRUE);
  if (pSetTheme) pSetTheme(h, L"DarkMode_Explorer", NULL);
}

// ---- DPI ------------------------------------------------------------------------------

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

// ---- helpers -----------------------------------------------------------------------

static void SetStatus(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  SetWindowTextA(g_status, buf);
}

static void SetStatusCol(const char* fmt, COLORREF col, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, col);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_status_col = col;
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
  SetStatusCol("%s", RGB(230, 120, 110), why);
}

static void ShowStopReply(const char* pkt) {
  if (pkt[0] == 'W' || pkt[0] == 'X') {
    g_state = ST_EXITED;
    UpdateButtons();
    SetStatusCol("guest exited (code %s)", RGB(230, 120, 110), pkt + 1);
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
  SetStatusCol("stopped (%s) - %d registers", RGB(231, 196, 113), pkt, g_tab.n);
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
    SetStatusCol("connect to %s failed (is cemu running with -s?)", RGB(230, 120, 110), hp);
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
  SetStatusCol("running - interrupt or wait for a breakpoint", RGB(134, 197, 102));
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
    SetStatusCol("breakpoint refused (%s)", RGB(230, 120, 110), pkt);
    return;
  }
  g_bps[g_nbps++] = addr;
  char text[32];
  snprintf(text, sizeof(text), "0x%llx", (unsigned long long)addr);
  SendMessageA(g_bplist, LB_ADDSTRING, 0, (LPARAM)text);
  SetStatusCol("breakpoint at %llx", RGB(134, 197, 102), (unsigned long long)addr);
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

// ---- window plumbing ---------------------------------------------------------------

static HWND MakeEdit(int id, int x, int y, int w, DWORD style) {
  HWND h = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | style, x, y, w, S(26),
                         g_wnd, (HMENU)(INT_PTR)id, NULL, NULL);
  SendMessageA(h, WM_SETFONT, (WPARAM)g_mono, TRUE);
  DarkModeChild(h);
  return h;
}

static HWND MakeLabel(const char* text, int x, int y, int w) {
  HWND h = CreateWindowA("STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, S(20), g_wnd, NULL,
                         NULL, NULL);
  SendMessageA(h, WM_SETFONT, (WPARAM)g_ui, TRUE);
  return h;
}

// Owner-drawn flat buttons: hover state tracked per control in its
// GWLP_USERDATA (1 = hot), paint happens in the parent's WM_DRAWITEM.
static LRESULT CALLBACK BtnProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                DWORD_PTR ref) {
  (void)id;
  (void)ref;
  switch (msg) {
    case WM_MOUSEMOVE: {
      if (!GetWindowLongPtr(h, GWLP_USERDATA)) {
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        SetWindowLongPtr(h, GWLP_USERDATA, 1);
        InvalidateRect(h, NULL, FALSE);
      }
      break;
    }
    case WM_MOUSELEAVE:
      SetWindowLongPtr(h, GWLP_USERDATA, 0);
      InvalidateRect(h, NULL, FALSE);
      break;
  }
  return DefSubclassProc(h, msg, wp, lp);
}

static HWND MakeBtnBase(const char* text, int id) {
  HWND h = CreateWindowA("BUTTON", text,
                         WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 0, 0, g_wnd,
                         (HMENU)(INT_PTR)id, NULL, NULL);
  SendMessageA(h, WM_SETFONT, (WPARAM)g_ui, TRUE);
  SetWindowSubclass(h, BtnProc, 0, 0);
  return h;
}

// ---- layout -------------------------------------------------------------------------
// Fixed-pixel panes (scaled by the DPI factor): toolbar row on top, registers
// pane on the left, memory pane filling the rest, breakpoints + hint strip
// along the bottom. Pane frames and captions are painted by WM_PAINT.
enum {
  kMargin = 10,
  kBottomH = 150,
  kLeftW = 310,
};

static void Relayout(void) {
  RECT rc;
  GetClientRect(g_wnd, &rc);
  int cw = rc.right, ch = rc.bottom;
  const int mem_x = S(kMargin + kLeftW + 8);
  const int pane_y = S(48);
  const int pane_h = ch - pane_y - S(kBottomH) - S(8);

  SetWindowPos(g_host, NULL, S(58), S(12), S(160), S(26), SWP_NOZORDER);
  SetWindowPos(g_connect, NULL, S(228), S(12), S(80), S(28), SWP_NOZORDER);
  SetWindowPos(g_detach, NULL, S(316), S(12), S(80), S(28), SWP_NOZORDER);
  SetWindowPos(g_run, NULL, S(428), S(12), S(92), S(28), SWP_NOZORDER);
  SetWindowPos(g_step, NULL, S(528), S(12), S(92), S(28), SWP_NOZORDER);
  SetWindowPos(g_stop, NULL, S(628), S(12), S(100), S(28), SWP_NOZORDER);
  SetWindowPos(g_status, NULL, cw - S(390), S(18), S(380), S(20), SWP_NOZORDER);

  SetWindowPos(g_regs, NULL, kMargin + S(8), pane_y + S(28), S(kLeftW) - S(16), pane_h - S(36),
               SWP_NOZORDER);
  SetWindowPos(g_memaddr, NULL, mem_x + S(14), pane_y + S(28), S(170), S(26), SWP_NOZORDER);
  SetWindowPos(g_goto, NULL, mem_x + S(192), pane_y + S(27), S(62), S(28), SWP_NOZORDER);
  SetWindowPos(g_follow, NULL, mem_x + S(260), pane_y + S(27), S(98), S(28), SWP_NOZORDER);
  SetWindowPos(g_mem, NULL, mem_x + S(14), pane_y + S(64), cw - mem_x - kMargin - S(28),
               pane_h - S(74), SWP_NOZORDER);

  const int by = ch - S(kBottomH);
  SetWindowPos(g_bplabel, NULL, kMargin + S(12), by + S(28), S(70), S(20), SWP_NOZORDER);
  SetWindowPos(g_bpaddr, NULL, kMargin + S(86), by + S(24), S(150), S(26), SWP_NOZORDER);
  SetWindowPos(g_bpadd, NULL, kMargin + S(246), by + S(23), S(88), S(28), SWP_NOZORDER);
  SetWindowPos(g_bpremove, NULL, kMargin + S(342), by + S(23), S(108), S(28), SWP_NOZORDER);
  SetWindowPos(g_bplist, NULL, kMargin + S(12), by + S(60), S(540), S(kBottomH) - S(72),
               SWP_NOZORDER);
  SetWindowPos(g_hint, NULL, mem_x + S(14), by + S(24), cw - mem_x - S(28), S(60), SWP_NOZORDER);
}

static void PaintPanes(HDC dc) {
  // pane frames + captions; the child controls paint over the interiors
  RECT rc;
  GetClientRect(g_wnd, &rc);
  int cw = rc.right, ch = rc.bottom;
  const int mem_x = S(kMargin + kLeftW + 8);
  const int pane_y = S(48);
  const int pane_h = ch - pane_y - S(kBottomH) - S(8);
  const int by = ch - S(kBottomH);
  HPEN pen = CreatePen(PS_SOLID, 1, kColPanelLine);
  HPEN old = (HPEN)SelectObject(dc, pen);
  HGDIOBJ oldf = SelectObject(dc, g_caption);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, kColCaption);
  struct {
    RECT r;
    const char* cap;
  } panes[3] = {
      {{kMargin, pane_y, kMargin + S(kLeftW), pane_y + pane_h}, "REGISTERS"},
      {{mem_x, pane_y, cw - kMargin, pane_y + pane_h}, "MEMORY"},
      {{kMargin, by, kMargin + S(568), by + S(kBottomH) - S(8)}, "BREAKPOINTS"},
  };
  for (int i = 0; i < 3; i++) {
    Rectangle(dc, panes[i].r.left, panes[i].r.top, panes[i].r.right, panes[i].r.bottom);
    TextOutA(dc, panes[i].r.left + S(10), panes[i].r.top + S(7), panes[i].cap,
             (int)strlen(panes[i].cap));
  }
  SelectObject(dc, oldf);
  SelectObject(dc, old);
  DeleteObject(pen);
  (void)cw;
}

// owner-drawn button paint (hover flag in the control's GWLP_USERDATA)
static void DrawButton(HDC dc, const DRAWITEMSTRUCT* di) {
  HWND h = di->hwndItem;
  int hot = (int)GetWindowLongPtr(h, GWLP_USERDATA);
  BOOL en = IsWindowEnabled(h);
  COLORREF bg = di->itemState & ODS_SELECTED ? kColBtnPress : (hot && en) ? kColBtnHover
                                                                          : kColBtnBg;
  HBRUSH br = CreateSolidBrush(bg);
  FillRect(dc, &di->rcItem, br);
  DeleteObject(br);
  if (en) {
    HPEN pen = CreatePen(PS_SOLID, 1, di->CtlID == IDC_CONNECT ? kColAccent : kColBtnLine);
    HPEN old = (HPEN)SelectObject(dc, pen);
    HBRUSH oldb = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, di->rcItem.left, di->rcItem.top, di->rcItem.right, di->rcItem.bottom);
    SelectObject(dc, oldb);
    SelectObject(dc, old);
    DeleteObject(pen);
  }
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, en ? kColBtnText : RGB(110, 110, 110));
  char text[48];
  GetWindowTextA(h, text, (int)sizeof(text));
  RECT tr = di->rcItem;
  HGDIOBJ oldf = SelectObject(dc, g_ui);
  DrawTextA(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  SelectObject(dc, oldf);
}

static void MakeFonts(void) {
  g_ui = CreateFontA(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                     "Segoe UI");
  g_mono = CreateFontA(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
                       "Consolas");
  g_caption = CreateFontA(-S(11), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
}

static void ApplyDpi(HWND wnd) {
  UINT (WINAPI *pGetDpi)(HWND) =
      (UINT (WINAPI*)(HWND))GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
  g_dpi = pGetDpi ? pGetDpi(wnd) : 96;
  if (g_dpi < 96) g_dpi = 96;
  if (g_ui) {
    DeleteObject(g_ui);
    DeleteObject(g_mono);
    DeleteObject(g_caption);
  }
  MakeFonts();
  HWND mono_ctrls[] = {g_regs, g_memaddr, g_mem, g_bpaddr, g_bplist};
  for (size_t i = 0; i < sizeof(mono_ctrls) / sizeof(mono_ctrls[0]); i++)
    SendMessageA(mono_ctrls[i], WM_SETFONT, (WPARAM)g_mono, TRUE);
  HWND ui_ctrls[] = {g_host, g_connect, g_detach, g_run, g_step, g_stop,
                     g_status, g_goto, g_follow, g_bpadd, g_bpremove,
                     g_bplabel, g_hint};
  for (size_t i = 0; i < sizeof(ui_ctrls) / sizeof(ui_ctrls[0]); i++)
    SendMessageA(ui_ctrls[i], WM_SETFONT, (WPARAM)g_ui, TRUE);
  Relayout();
  InvalidateRect(wnd, NULL, TRUE);
}

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      g_wnd = wnd;
      DarkModeWindow(wnd);
      MakeFonts();
      g_host = MakeEdit(IDC_HOST, 0, 0, 0, ES_AUTOHSCROLL);
      SetWindowTextA(g_host, "127.0.0.1:1234");
      g_connect = MakeBtnBase("Connect", IDC_CONNECT);
      g_detach = MakeBtnBase("Detach", IDC_DETACH);
      g_run = MakeBtnBase("Run (F5)", IDC_RUN);
      g_step = MakeBtnBase("Step (F10)", IDC_STEP);
      g_stop = MakeBtnBase("Interrupt", IDC_STOP);
      g_status = MakeLabel("disconnected", 0, 0, 0);
      SetWindowLongPtr(g_status, GWL_STYLE, GetWindowLongPtr(g_status, GWL_STYLE) | SS_RIGHT);
      g_regs = CreateWindowExA(0, WC_LISTVIEWA, "",
                               WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT |
                                   LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                               0, 0, 0, 0, wnd, (HMENU)(INT_PTR)IDC_REGS, NULL, NULL);
      SendMessageA(g_regs, WM_SETFONT, (WPARAM)g_mono, TRUE);
      DarkModeChild(g_regs);
      ListView_SetExtendedListViewStyle(g_regs, LVS_EX_FULLROWSELECT);
      ListView_SetBkColor(g_regs, kColEditBg);
      ListView_SetTextBkColor(g_regs, kColEditBg);
      ListView_SetTextColor(g_regs, kColEditText);
      LVCOLUMNA col;
      memset(&col, 0, sizeof(col));
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = "register";
      col.cx = S(120);
      ListView_InsertColumn(g_regs, 0, &col);
      col.pszText = "value";
      col.cx = S(150);
      ListView_InsertColumn(g_regs, 1, &col);
      HWND hdr = FindWindowExA(g_regs, NULL, "SysHeader32", NULL);
      if (hdr) DarkModeChild(hdr);
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
      DarkModeChild(g_bplist);
      g_hint = MakeLabel("no disassembler by design - decode with external\r\n"
                         "tools (llvm-objdump, or gdb on the same stub)",
                         0, 0, 0);
      SetTimer(wnd, 1, 100, NULL);
      ApplyDpi(wnd);
      UpdateButtons();
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      PaintPanes(dc);
      EndPaint(wnd, &ps);
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
    case WM_DRAWITEM:
      DrawButton((HDC)wp, (const DRAWITEMSTRUCT*)lp);
      return TRUE;
    case WM_CTLCOLORSTATIC: {
      // read-only edits (the memory view) and labels paint here
      HDC dc = (HDC)wp;
      SetBkMode(dc, TRANSPARENT);
      if (GetDlgCtrlID((HWND)lp) == IDC_MEM) {
        SetTextColor(dc, kColMemText);
        static HBRUSH mem_br;
        if (!mem_br) mem_br = CreateSolidBrush(kColEditBg);
        return (LRESULT)mem_br;
      }
      SetTextColor(dc, GetDlgCtrlID((HWND)lp) == IDC_STATUS ? g_status_col
                       : !GetDlgCtrlID((HWND)lp)            ? kColCaption  // hint
                                                            : RGB(200, 200, 200));
      static HBRUSH bg_br;
      if (!bg_br) bg_br = CreateSolidBrush(kColBg);
      return (LRESULT)bg_br;
    }
    case WM_CTLCOLOREDIT: {
      HDC dc = (HDC)wp;
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, kColEditText);
      static HBRUSH edit_br;
      if (!edit_br) edit_br = CreateSolidBrush(kColEditBg);
      return (LRESULT)edit_br;
    }
    case WM_CTLCOLORLISTBOX: {
      HDC dc = (HDC)wp;
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, kColEditText);
      static HBRUSH list_br;
      if (!list_br) list_br = CreateSolidBrush(kColEditBg);
      return (LRESULT)list_br;
    }
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
      mmi->ptMinTrackSize.x = S(980);
      mmi->ptMinTrackSize.y = S(640);
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
  DarkModeInit();

  INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES};
  InitCommonControlsEx(&icc);

  WNDCLASSA wc;
  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(kColBg);
  wc.lpszClassName = "cemugui";
  RegisterClassA(&wc);

  // F5 = run, F10 = step (accelerators reach the buttons even when focus
  // sits in an edit box)
  ACCEL acc[2] = {{FVIRTKEY, VK_F5, IDC_RUN}, {FVIRTKEY, VK_F10, IDC_STEP}};
  HACCEL acc_tab = CreateAcceleratorTableA(acc, 2);

  HWND wnd = CreateWindowA(
      "cemugui", "cemugui - cemu debug front (no disassembler: use llvm-objdump / gdb)",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1160, 720, NULL, NULL, inst, NULL);
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
