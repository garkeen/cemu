// Win32 GDI display backend (阶段 3.5 片 2, host/ is the only windows.h user).
// One fixed-size window showing a device-owned XRGB buffer 1:1: WM_PAINT blits it with
// StretchDIBits, and HostDisplayPump — called from the board run loop — runs the message
// queue and invalidates on version changes. The window is deliberately not resizable:
// the emulated machine's display has exactly one size, the device framebuffer's.
// Everything lives on the emulator's main thread, so there is no cross-thread state.
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
  void (*key_cb)(void* ctx, uint32_t scan, int extended, int up);
  void* key_ctx;
  void (*mouse_cb)(void* ctx, int dx, int dy, int dz, int buttons);
  void* mouse_ctx;
  // Pointer tracking: the PS/2 packet carries movement, not a position, so
  // the window reports each WM_MOUSEMOVE as a delta from the previous one.
  // `mouse_valid` is cleared when the pointer leaves the client area (or the
  // window loses focus), so re-entering does not read as one huge jump.
  int mouse_x, mouse_y, mouse_valid, mouse_buttons;
  uint32_t shown_version;
  int64_t next_pump_us;
  int closed;
  BITMAPINFO bi;
};

// The only blit path: 1:1 (the client area is exactly the device framebuffer, because
// the window is not resizable), and it covers the whole client area — which is why
// WM_ERASEBKGND below can stay a no-op. The version counter is consumed here so the
// pump's invalidate loop is quiescent until the device publishes a new frame.
static void Blit(HostDisplay* d, HDC dc) {
  SetStretchBltMode(dc, COLORONCOLOR);
  StretchDIBits(dc, 0, 0, d->width, d->height, 0, 0, d->width, d->height, d->fb, &d->bi,
                DIB_RGB_COLORS, SRCCOPY);
  d->shown_version = d->version_cb(d->dev);
}

static LRESULT CALLBACK DisplayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  HostDisplay* d = (HostDisplay*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      Blit(d, dc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;  // the blit covers the client area; avoid flicker
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      if (!d->key_cb) return DefWindowProcA(hwnd, msg, wp, lp);  // Alt+F4 etc. stay
      // lp bits 16-23 carry the keyboard's own make code — the set-1 byte the
      // PC/AT sends — and bit 24 marks the 0xe0-prefixed keys (Win32
      // WM_KEYDOWN). The model adds the break bit from `up`.
      uint32_t scan = (uint32_t)((lp >> 16) & 0xff);
      int up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
      d->key_cb(d->key_ctx, scan, (int)((lp >> 24) & 1), up);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!d->mouse_cb) return DefWindowProcA(hwnd, msg, wp, lp);
      int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
      if (d->mouse_valid)
        d->mouse_cb(d->mouse_ctx, x - d->mouse_x, y - d->mouse_y, 0, d->mouse_buttons);
      d->mouse_x = x;
      d->mouse_y = y;
      d->mouse_valid = 1;
      // Ask for WM_MOUSELEAVE so the delta base is dropped when the pointer
      // leaves; Win32 sends it once per TrackMouseEvent call.
      TRACKMOUSEEVENT tme;
      memset(&tme, 0, sizeof(tme));
      tme.cbSize = sizeof(tme);
      tme.dwFlags = TME_LEAVE;
      tme.hwndTrack = hwnd;
      TrackMouseEvent(&tme);
      return 0;
    }
    case WM_MOUSELEAVE:
      d->mouse_valid = 0;
      return 0;
    case WM_KILLFOCUS:
      // Focus going away costs us the pointer too: without this the next
      // WM_MOUSEMOVE after the user comes back would report one huge delta.
      d->mouse_valid = 0;
      return DefWindowProcA(hwnd, msg, wp, lp);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
      if (!d->mouse_cb) return DefWindowProcA(hwnd, msg, wp, lp);
      // Button state after the event: bit 0 left, bit 1 right, bit 2 middle —
      // the order the PS/2 packet's first byte uses.
      int bit = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)     ? 0x01
                : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP)   ? 0x02
                                                                   : 0x04;
      int down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
      if (down)
        d->mouse_buttons |= bit;
      else
        d->mouse_buttons &= ~bit;
      // A press or release with no movement still reaches the guest: the
      // packet carries the buttons, and the device sends one for the event.
      d->mouse_cb(d->mouse_ctx, 0, 0, 0, d->mouse_buttons);
      // Keep reporting while a button is held even if the pointer leaves the
      // client area — that is what a drag is.
      if (down)
        SetCapture(hwnd);
      else if (!d->mouse_buttons)
        ReleaseCapture();
      return 0;
    }
    case WM_MOUSEWHEEL: {
      if (!d->mouse_cb) return DefWindowProcA(hwnd, msg, wp, lp);
      // The wheel arrives in multiples of WHEEL_DELTA; the PS/2 wheel byte
      // counts detents, so one notch is one.
      int dz = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
      if (dz) d->mouse_cb(d->mouse_ctx, 0, 0, dz, d->mouse_buttons);
      return 0;
    }
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

  // Fixed size on purpose: the emulated machine's display has exactly one size, so the
  // window drops WS_THICKFRAME/WS_MAXIMIZEBOX and the client area is always the device
  // framebuffer — which is what keeps the WM_PAINT blit 1:1.
  static const DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT rc = {0, 0, width, height};
  AdjustWindowRect(&rc, kStyle, FALSE);
  d->hwnd = CreateWindowA("cemu_display", title, kStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                          rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance,
                          NULL);
  if (!d->hwnd) {
    LogError("display: CreateWindow failed");
    free(d);
    return NULL;
  }
  SetWindowLongPtr(d->hwnd, GWLP_USERDATA, (LONG_PTR)d);
  ShowWindow(d->hwnd, SW_SHOWNORMAL);
  SetForegroundWindow(d->hwnd);  // keys land in the focused window
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
  // Invalidate only: WM_PAINT is the single blit path, so one published frame costs one
  // blit instead of the old immediate blit plus the paint that followed it.
  if (d->version_cb(d->dev) != d->shown_version) InvalidateRect(d->hwnd, NULL, FALSE);
}

int HostDisplayClosed(const HostDisplay* d) { return d && d->closed; }

void HostDisplaySetKeySink(HostDisplay* d,
                           void (*cb)(void* ctx, uint32_t scan, int extended, int up),
                           void* ctx) {
  d->key_cb = cb;
  d->key_ctx = ctx;
}

void HostDisplaySetMouseSink(HostDisplay* d,
                             void (*cb)(void* ctx, int dx, int dy, int dz, int buttons),
                             void* ctx) {
  d->mouse_cb = cb;
  d->mouse_ctx = ctx;
}

void HostDisplayFree(HostDisplay* d) {
  if (!d) return;
  if (d->hwnd) DestroyWindow(d->hwnd);
  free(d);
}
