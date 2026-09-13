// Win32 GDI display backend (阶段 3.5 片 2, host/ is the only windows.h user).
// One window showing a device-owned XRGB buffer: WM_PAINT blits it with
// StretchDIBits, and HostDisplayPump — called from the board run loop — runs
// the message queue and invalidates on version changes. Everything lives on
// the emulator's main thread, so there is no cross-thread state.
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "host/host.h"
#include "util/log.h"

// Pump cadence: process messages / check for repaints at most this often.
enum { kPumpIntervalUs = 4000 };

struct HostDisplay {
  HWND hwnd;
  int width;
  int height;
  const uint32_t* fb;
  uint32_t (*version_cb)(void* dev);
  void* dev;
  uint32_t shown_version;
  int64_t next_pump_us;
  int closed;
  BITMAPINFO bi;
};

static void Blit(HostDisplay* d) {
  HDC dc = GetDC(d->hwnd);
  if (!dc) return;
  RECT rc;
  GetClientRect(d->hwnd, &rc);
  SetStretchBltMode(dc, COLORONCOLOR);
  StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, d->width, d->height, d->fb,
                &d->bi, DIB_RGB_COLORS, SRCCOPY);
  ReleaseDC(d->hwnd, dc);
  d->shown_version = d->version_cb(d->dev);
}

static LRESULT CALLBACK DisplayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  HostDisplay* d = (HostDisplay*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      SetStretchBltMode(dc, COLORONCOLOR);
      StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, d->width, d->height,
                    d->fb, &d->bi, DIB_RGB_COLORS, SRCCOPY);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;  // the blit covers the client area; avoid flicker
    case WM_SIZE:
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      d->closed = 1;
      return 0;
    default:
      return DefWindowProcA(hwnd, msg, wp, lp);
  }
}

HostDisplay* HostDisplayOpen(const char* title, int width, int height,
                             const uint32_t* fb,
                             uint32_t (*version_cb)(void* dev), void* dev) {
  HostDisplay* d = (HostDisplay*)calloc(1, sizeof(*d));
  if (!d) return NULL;
  d->width = width;
  d->height = height;
  d->fb = fb;
  d->version_cb = version_cb;
  d->dev = dev;
  BITMAPINFOHEADER* h = &d->bi.bmiHeader;
  h->biSize = sizeof(*h);
  h->biWidth = width;
  h->biHeight = -height;  // top-down rows
  h->biPlanes = 1;
  h->biBitCount = 32;
  h->biCompression = BI_RGB;  // 0xffRRGGBB little-endian = B,G,R,255 bytes

  WNDCLASSA wc;
  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = DisplayWndProc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.lpszClassName = "cemu_display";
  RegisterClassA(&wc);

  RECT rc = {0, 0, width, height};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  d->hwnd = CreateWindowA("cemu_display", title, WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                          rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  if (!d->hwnd) {
    LogError("display: CreateWindow failed");
    free(d);
    return NULL;
  }
  SetWindowLongPtr(d->hwnd, GWLP_USERDATA, (LONG_PTR)d);
  ShowWindow(d->hwnd, SW_SHOWNORMAL);
  return d;
}

void HostDisplayPump(HostDisplay* d) {
  if (!d || d->closed) return;
  int64_t now = HostTimerNow();
  if (now < d->next_pump_us) return;
  d->next_pump_us = now + kPumpIntervalUs;
  MSG msg;
  while (PeekMessageA(&msg, d->hwnd, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  if (d->closed) return;
  if (d->version_cb(d->dev) != d->shown_version) {
    InvalidateRect(d->hwnd, NULL, FALSE);
    Blit(d);  // paint now; the WM_PAINT that follows is a cheap no-op repaint
  }
}

int HostDisplayClosed(const HostDisplay* d) { return d && d->closed; }

void HostDisplayFree(HostDisplay* d) {
  if (!d) return;
  if (d->hwnd) DestroyWindow(d->hwnd);
  free(d);
}
