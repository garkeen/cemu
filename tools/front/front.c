// cemugui — the cemu gdb-stub front-end (阶段 3.5 片 3). A pure RSP client:
// register / memory views, breakpoints, run-step-stop control. There is no
// disassembler here by decision (2026-09-12): instruction bytes are shown in
// the memory view and decoded with an external tool (llvm-objdump, or gdb /
// lldb attached to the same stub).
//
// Single-threaded Win32, the same shape as the display window: a 100ms timer
// polls the socket while the guest runs (stop replies only arrive then);
// everything else is a request/response exchange while stopped, which is
// sub-millisecond against a local stub.
#include <windows.h>
#include <commctrl.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsp.h"

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
  IDC_HINT,
};

// connection state
enum { ST_DOWN = 0, ST_STOPPED, ST_RUNNING, ST_EXITED };

enum { kMemRows = 16, kMaxBps = 64 };  // 256 bytes per fetch fits PacketSize

static Rsp* g_rsp;
static RspRegTable g_tab;
static int g_state = ST_DOWN;
static uint64_t g_mem_addr;
static uint64_t g_pc;
static uint64_t g_bps[kMaxBps];
static int g_nbps;

static HWND g_wnd, g_host, g_connect, g_detach, g_run, g_step, g_stop, g_status;
static HWND g_grp_regs, g_regs, g_grp_mem, g_memaddr, g_goto, g_follow, g_mem;
static HWND g_grp_bp, g_bplabel, g_bpaddr, g_bpadd, g_bpremove, g_bplist, g_hint;
static HFONT g_mono, g_ui;

static void RefreshRegs(void);
static void RefreshMem(void);
static void ShowStopReply(const char* pkt);

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

// ---- window plumbing ---------------------------------------------------------------

static HWND MakeBtn(const char* text, int id, int x, int y, int w) {
  HWND h = CreateWindowA("BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, y, w, 26,
                         g_wnd, (HMENU)(INT_PTR)id, NULL, NULL);
  SendMessageA(h, WM_SETFONT, (WPARAM)g_ui, TRUE);
  return h;
}

static HWND MakeEdit(int id, int x, int y, int w, DWORD style) {
  HWND h = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | style, x, y, w, 24,
                         g_wnd, (HMENU)(INT_PTR)id, NULL, NULL);
  SendMessageA(h, WM_SETFONT, (WPARAM)g_mono, TRUE);
  return h;
}

static HWND MakeLabel(const char* text, int x, int y, int w) {
  HWND h = CreateWindowA("STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, 20, g_wnd, NULL, NULL,
                         NULL);
  SendMessageA(h, WM_SETFONT, (WPARAM)g_ui, TRUE);
  return h;
}

static HWND MakeGroup(const char* text, int x, int y, int w, int h) {
  // BS_GROUPBOX frame; created BEFORE its children so they paint on top.
  HWND g =
      CreateWindowA("BUTTON", text, WS_CHILD | WS_VISIBLE | BS_GROUPBOX, x, y, w, h, g_wnd,
                    NULL, NULL, NULL);
  SendMessageA(g, WM_SETFONT, (WPARAM)g_ui, TRUE);
  return g;
}

// ---- layout -------------------------------------------------------------------------
// Fixed-pixel panes: toolbar row on top, registers pane on the left, memory
// pane filling the rest, breakpoints + hint strip along the bottom.
enum {
  kMargin = 8,
  kBottomH = 150,
  kLeftW = 300,
};

static void Relayout(void) {
  RECT rc;
  GetClientRect(g_wnd, &rc);
  int cw = rc.right, ch = rc.bottom;
  const int mem_x = kMargin + kLeftW + 8;
  const int pane_y = 48;
  const int pane_h = ch - pane_y - kBottomH - 8;

  // toolbar
  SetWindowPos(g_host, NULL, 56, 12, 160, 24, SWP_NOZORDER);
  SetWindowPos(g_connect, NULL, 224, 11, 78, 26, SWP_NOZORDER);
  SetWindowPos(g_detach, NULL, 308, 11, 78, 26, SWP_NOZORDER);
  SetWindowPos(g_run, NULL, 414, 11, 88, 26, SWP_NOZORDER);
  SetWindowPos(g_step, NULL, 508, 11, 88, 26, SWP_NOZORDER);
  SetWindowPos(g_stop, NULL, 602, 11, 96, 26, SWP_NOZORDER);
  SetWindowPos(g_status, NULL, cw - 370, 17, 360, 20, SWP_NOZORDER);

  // registers pane
  SetWindowPos(g_grp_regs, NULL, kMargin, pane_y, kLeftW, pane_h, SWP_NOZORDER);
  SetWindowPos(g_regs, NULL, kMargin + 8, pane_y + 22, kLeftW - 16, pane_h - 30, SWP_NOZORDER);

  // memory pane
  SetWindowPos(g_grp_mem, NULL, mem_x, pane_y, cw - mem_x - kMargin, pane_h, SWP_NOZORDER);
  SetWindowPos(g_memaddr, NULL, mem_x + 14, pane_y + 22, 170, 24, SWP_NOZORDER);
  SetWindowPos(g_goto, NULL, mem_x + 190, pane_y + 21, 60, 26, SWP_NOZORDER);
  SetWindowPos(g_follow, NULL, mem_x + 256, pane_y + 21, 96, 26, SWP_NOZORDER);
  SetWindowPos(g_mem, NULL, mem_x + 14, pane_y + 56, cw - mem_x - kMargin - 28,
               pane_h - 66, SWP_NOZORDER);

  // breakpoints strip + hint
  const int by = ch - kBottomH;
  SetWindowPos(g_grp_bp, NULL, kMargin, by, 560, kBottomH - 8, SWP_NOZORDER);
  SetWindowPos(g_bplabel, NULL, kMargin + 12, by + 26, 70, 20, SWP_NOZORDER);
  SetWindowPos(g_bpaddr, NULL, kMargin + 86, by + 22, 150, 24, SWP_NOZORDER);
  SetWindowPos(g_bpadd, NULL, kMargin + 244, by + 21, 84, 26, SWP_NOZORDER);
  SetWindowPos(g_bpremove, NULL, kMargin + 334, by + 21, 100, 26, SWP_NOZORDER);
  SetWindowPos(g_bplist, NULL, kMargin + 12, by + 54, 536, kBottomH - 70, SWP_NOZORDER);
  SetWindowPos(g_hint, NULL, 580, by + 12, cw - 588, 60, SWP_NOZORDER);
}

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      g_mono = CreateFontA(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
                           "Consolas");
      g_ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
      g_wnd = wnd;
      MakeLabel("target:", 8, 12, 46);
      g_host = MakeEdit(IDC_HOST, 56, 8, 150, ES_AUTOHSCROLL);
      SetWindowTextA(g_host, "127.0.0.1:1234");
      g_connect = MakeBtn("Connect", IDC_CONNECT, 212, 7, 76);
      g_detach = MakeBtn("Detach", IDC_DETACH, 292, 7, 76);
      g_run = MakeBtn("Run (F5)", IDC_RUN, 384, 7, 84);
      g_step = MakeBtn("Step (F10)", IDC_STEP, 472, 7, 84);
      g_stop = MakeBtn("Interrupt", IDC_STOP, 560, 7, 96);
      g_status = MakeLabel("disconnected", 664, 17, 400);
      // group frames first: later siblings paint on top of them
      g_grp_regs = MakeGroup("registers", kMargin, 48, kLeftW, 100);
      g_grp_mem = MakeGroup("memory", 316, 48, 400, 100);
      g_grp_bp = MakeGroup("breakpoints", kMargin, 400, 560, 142);
      g_regs = CreateWindowExA(0, WC_LISTVIEWA, "",
                               WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT |
                                   LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                               16, 70, 284, 300, wnd, (HMENU)(INT_PTR)IDC_REGS, NULL, NULL);
      SendMessageA(g_regs, WM_SETFONT, (WPARAM)g_mono, TRUE);
      LVCOLUMNA col;
      memset(&col, 0, sizeof(col));
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = "register";
      col.cx = 120;
      ListView_InsertColumn(g_regs, 0, &col);
      col.pszText = "value";
      col.cx = 150;
      ListView_InsertColumn(g_regs, 1, &col);
      g_memaddr = MakeEdit(IDC_MEMADDR, 0, 0, 0, ES_AUTOHSCROLL);
      g_goto = MakeBtn("Goto", IDC_GOTO, 0, 0, 56);
      g_follow = MakeBtn("Follow PC", IDC_FOLLOW, 0, 0, 92);
      g_mem = MakeEdit(IDC_MEM, 0, 0, 0,
                       ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);
      g_bplabel = MakeLabel("addr:", 20, 0, 70);
      g_bpaddr = MakeEdit(IDC_BPADDR, 0, 0, 0, ES_AUTOHSCROLL);
      g_bpadd = MakeBtn("Add (Z0)", IDC_BPADD, 0, 0, 84);
      g_bpremove = MakeBtn("Remove (z0)", IDC_BPREMOVE, 0, 0, 100);
      g_bplist = CreateWindowA("LISTBOX", "",
                               WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                                   LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                               0, 0, 0, 0, wnd, (HMENU)(INT_PTR)IDC_BPLIST, NULL, NULL);
      SendMessageA(g_bplist, WM_SETFONT, (WPARAM)g_mono, TRUE);
      g_hint = CreateWindowA(
          "STATIC",
          "no disassembler by design - decode with external\r\ntools (llvm-objdump, or gdb "
          "on the same stub)",
          WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, wnd, (HMENU)(INT_PTR)IDC_HINT, NULL, NULL);
      SendMessageA(g_hint, WM_SETFONT, (WPARAM)g_ui, TRUE);
      SetTimer(wnd, 1, 100, NULL);
      UpdateButtons();
      return 0;
    }
    case WM_SIZE:
      Relayout();
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
      mmi->ptMinTrackSize.x = 980;
      mmi->ptMinTrackSize.y = 640;
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
