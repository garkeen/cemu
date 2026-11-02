// Win32 GDI display backend (阶段 3.5 片 2, host/ is the only windows.h user).
// One resizable window showing a device-owned XRGB buffer: WM_PAINT scales the
// picture into the client area (fit, never crop; the rest is black), and
// HostDisplayPump — called from the board run loop — runs the message queue and
// invalidates on version changes. The pointer contract is absolute: a point in
// the client maps to the picture's source pixel, letterbox points clamp to the
// nearest edge, and points outside the client report nothing; the picture rect
// is recomputed per event, so resizing needs no cached geometry. Everything
// lives on the emulator's main thread, so there is no cross-thread state.
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "host/host.h"
#include "device/video/display.h"  // the device-side vtable this window calls
#include "util/log.h"

// Pump cadence: process messages / check for repaints at most this often.
enum { kPumpIntervalUs = 4000 };
// How close to the client edge the grabbed pointer may get before it is
// warped back to the center. The movement that reached the edge is still
// reported; the warp itself is not (the base is reset to the center).
enum { kGrabMargin = 8 };

struct HostDisplay {
  HWND hwnd;
  // The device's vtable (device/video/display.h): the size, the pixel buffer
  // and the change counter all come from it, because a card whose mode changes
  // (the VGA family core) changes its picture's size at runtime.
  const DisplaySourceOps* ops;
  void* dev;
  int width;   // the picture's size the window currently shows
  int height;
  void (*key_cb)(void* ctx, uint32_t scan, int extended, int up);
  void* key_ctx;
  void (*mouse_cb)(void* ctx, int dx, int dy, int dz, int buttons);
  void* mouse_ctx;
  void (*mouse_abs_cb)(void* ctx, int x, int y, int dz, int buttons);
  void* mouse_abs_ctx;
  // Pointer tracking: the PS/2 packet carries movement, not a position, so
  // the relative sink reports each WM_MOUSEMOVE as a delta from the previous
  // one. `mouse_valid` is cleared when the pointer leaves the client area (or
  // the window loses focus), so re-entering does not read as one huge jump.
  // The absolute sink keeps the last valid picture position instead; button
  // and wheel events outside the client (capture) still have a position to
  // report.
  int mouse_x, mouse_y, mouse_valid, mouse_buttons;
  // Relative channel: fractional source-picture counts carried between host
  // events. The picture is scaled into the client by ClientPictureRect; a
  // relative device has to apply the same scale or the guest cursor moves
  // faster/slower than the host pointer when the window is resized (a
  // maximized 1024x768 picture on a 1080p screen is ~1.4x).
  int64_t rel_acc_x, rel_acc_y;
  int abs_x, abs_y, abs_valid;
  // Relative-only pointer grab (D38): the host cursor is hidden and warped
  // back to the client center when it nears an edge, so the guest can keep
  // moving past its own screen edge and the host pointer never runs out of
  // window. Ctrl+Alt (or focus loss) releases it.
  int grabbed;
  int cursor_hidden;
  int focused;
  int ctrl_down, alt_down;  // Ctrl+Alt releases the grab
  int grab_suppressed;      // released: wait for a click before grabbing again
  uint32_t shown_version;
  int64_t next_pump_us;
  int closed;
  BITMAPINFO bi;
  // Off-screen buffer for WM_PAINT: FillRect(black) followed by
  // StretchDIBits directly on the window DC shows the black between the two
  // calls, which reads as a full-screen flash on every published frame.
  HDC back_dc;
  HBITMAP back_bmp;
  int back_w, back_h;
};

// The client rect and the picture rect inside it, using the same fit rule as
// Blit (scale by whichever axis runs out first; never crop). Recomputed on
// every call so window resizing needs no cached geometry.
static int ClientPictureRect(HostDisplay* d, RECT* client, RECT* pic) {
  GetClientRect(d->hwnd, client);
  int cw = client->right - client->left, ch = client->bottom - client->top;
  if (cw <= 0 || ch <= 0 || d->width <= 0 || d->height <= 0) return 0;
  const int fit_width = (int64_t)cw * d->height <= (int64_t)ch * d->width;
  const int num = fit_width ? cw : ch;
  const int den = fit_width ? d->width : d->height;
  const int w = (int)((int64_t)d->width * num / den);
  const int h = (int)((int64_t)d->height * num / den);
  if (w <= 0 || h <= 0) return 0;
  pic->left = (cw - w) / 2;
  pic->top = (ch - h) / 2;
  pic->right = pic->left + w;
  pic->bottom = pic->top + h;
  return 1;
}

// Client point -> source-picture pixel (X right, Y down). Points in the
// letterbox clamp to the nearest picture edge; points outside the client rect
// are not reported (the caller sends nothing).
static int MapPointer(HostDisplay* d, int cx, int cy, int* sx, int* sy) {
  RECT client, pic;
  if (!ClientPictureRect(d, &client, &pic)) return 0;
  if (cx < client.left || cx >= client.right || cy < client.top || cy >= client.bottom) return 0;
  int pw = pic.right - pic.left, ph = pic.bottom - pic.top;
  int x, y;
  if (cx < pic.left)
    x = 0;
  else if (cx >= pic.right)
    x = d->width - 1;
  else
    x = (int)((int64_t)(cx - pic.left) * d->width / pw);
  if (cy < pic.top)
    y = 0;
  else if (cy >= pic.bottom)
    y = d->height - 1;
  else
    y = (int)((int64_t)(cy - pic.top) * d->height / ph);
  if (x < 0) x = 0;
  if (x >= d->width) x = d->width - 1;
  if (y < 0) y = 0;
  if (y >= d->height) y = d->height - 1;
  *sx = x;
  *sy = y;
  return 1;
}

// Client-pixel delta -> source-picture counts. The relative channel reports a
// delta, not a position, but the picture is scaled uniformly into the client;
// applying the same scale is what keeps the guest cursor under the host
// pointer's displacement when the window is resized. The 16.16 accumulator
// keeps the fraction of a count that a small movement leaves over.
static void RelativeDelta(HostDisplay* d, int cdx, int cdy, int* gdx, int* gdy) {
  *gdx = cdx;
  *gdy = cdy;
  RECT client, pic;
  if (!ClientPictureRect(d, &client, &pic)) {
    d->rel_acc_x = 0;
    d->rel_acc_y = 0;
    return;
  }
  int pw = pic.right - pic.left, ph = pic.bottom - pic.top;
  if (pw <= 0 || ph <= 0) return;
  int64_t sx = ((int64_t)d->width << 16) / pw;
  int64_t sy = ((int64_t)d->height << 16) / ph;
  d->rel_acc_x += (int64_t)cdx * sx;
  d->rel_acc_y += (int64_t)cdy * sy;
  *gdx = (int)(d->rel_acc_x / 65536);
  *gdy = (int)(d->rel_acc_y / 65536);
  d->rel_acc_x -= (int64_t)*gdx * 65536;
  d->rel_acc_y -= (int64_t)*gdy * 65536;
}

// Put the hidden pointer back at the client center so the next movement has
// room in every direction. The warp itself must not become a relative delta:
// mouse_x/mouse_y are set to the same center point.
static void RecenterPointer(HostDisplay* d) {
  RECT rc;
  GetClientRect(d->hwnd, &rc);
  POINT p;
  p.x = (rc.left + rc.right) / 2;
  p.y = (rc.top + rc.bottom) / 2;
  ClientToScreen(d->hwnd, &p);
  SetCursorPos(p.x, p.y);
  d->mouse_x = (rc.left + rc.right) / 2;
  d->mouse_y = (rc.top + rc.bottom) / 2;
  d->mouse_valid = 1;
  d->rel_acc_x = 0;
  d->rel_acc_y = 0;
}

static int NearClientEdge(HostDisplay* d, int x, int y) {
  RECT rc;
  GetClientRect(d->hwnd, &rc);
  int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 2 * kGrabMargin || h <= 2 * kGrabMargin) return 1;
  return x < rc.left + kGrabMargin || x >= rc.right - kGrabMargin ||
         y < rc.top + kGrabMargin || y >= rc.bottom - kGrabMargin;
}

// Relative-only pointer grab (D38). PS/2 has no absolute packet, so the guest
// cursor is moved by counts; without a grab the host pointer can reach the
// window edge while the guest cursor is still in the middle (or vice versa),
// and the lost movement comes back as a jump when the pointer re-enters. Hide
// the host cursor, capture the pointer, and warp it to the center near an
// edge: the guest gets unlimited relative movement and the only visible cursor
// is the guest's own. Ctrl+Alt (or losing focus) releases it.
static void GrabPointer(HostDisplay* d) {
  if (d->grabbed || !d->mouse_cb || d->mouse_abs_cb) return;
  d->grabbed = 1;
  d->grab_suppressed = 0;
  d->mouse_valid = 0;
  d->rel_acc_x = 0;
  d->rel_acc_y = 0;
  d->ctrl_down = 0;
  d->alt_down = 0;
  SetCapture(d->hwnd);
  if (!d->cursor_hidden) {
    ShowCursor(FALSE);
    d->cursor_hidden = 1;
  }
  RecenterPointer(d);
}

static void ReleasePointer(HostDisplay* d) {
  if (!d->grabbed) {
    if (d->cursor_hidden) {
      ShowCursor(TRUE);
      d->cursor_hidden = 0;
    }
    return;
  }
  d->grabbed = 0;
  d->grab_suppressed = 1;  // no auto-grab until the user clicks the display
  if (GetCapture() == d->hwnd) ReleaseCapture();
  if (d->cursor_hidden) {
    ShowCursor(TRUE);
    d->cursor_hidden = 0;
  }
  d->mouse_valid = 0;
  d->rel_acc_x = 0;
  d->rel_acc_y = 0;
}

// The only blit path: the picture is scaled to the client area, preserving its
// aspect ratio, and the area around it is black. The window is resizable (a
// VGA's modes come in several sizes) and the version counter is consumed here
// so the pump's invalidate loop is quiescent until a new frame is published.
static void Blit(HostDisplay* d, HDC dc) {
  RECT rc, pic;
  if (!ClientPictureRect(d, &rc, &pic)) return;
  int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
  if (d->back_w != cw || d->back_h != ch || !d->back_dc) {
    if (d->back_dc) DeleteDC(d->back_dc);
    if (d->back_bmp) DeleteObject(d->back_bmp);
    d->back_dc = CreateCompatibleDC(dc);
    d->back_bmp = CreateCompatibleBitmap(dc, cw, ch);
    if (d->back_dc && d->back_bmp) {
      SelectObject(d->back_dc, d->back_bmp);
      d->back_w = cw;
      d->back_h = ch;
    } else {
      if (d->back_dc) DeleteDC(d->back_dc);
      if (d->back_bmp) DeleteObject(d->back_bmp);
      d->back_dc = NULL;
      d->back_bmp = NULL;
      d->back_w = d->back_h = 0;
    }
  }
  HDC target = d->back_dc ? d->back_dc : dc;
  HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
  FillRect(target, &rc, black);
  SetStretchBltMode(target, COLORONCOLOR);
  StretchDIBits(target, pic.left, pic.top, pic.right - pic.left, pic.bottom - pic.top, 0, 0,
                d->width, d->height, d->ops->Framebuffer(d->dev), &d->bi, DIB_RGB_COLORS,
                SRCCOPY);
  if (target != dc) BitBlt(dc, 0, 0, cw, ch, target, 0, 0, SRCCOPY);
  d->shown_version = d->ops->Version(d->dev);
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
      return 1;  // Blit paints the whole client area (picture plus letterbox)
    case WM_SETFOCUS:
      d->focused = 1;
      d->mouse_valid = 0;
      d->rel_acc_x = 0;
      d->rel_acc_y = 0;
      return 0;
    case WM_SETCURSOR:
      // A grabbed relative pointer has no visible host cursor: the guest
      // draws its own. Outside the client (title bar/border) the normal arrow
      // comes back, which is also the way to reach the window buttons after
      // releasing the grab.
      if (d->grabbed && LOWORD(lp) == HTCLIENT) {
        SetCursor(NULL);
        return TRUE;
      }
      return DefWindowProcA(hwnd, msg, wp, lp);
    case WM_SIZE:
      // The client geometry changed: the old mouse_x/mouse_y are in the old
      // coordinate system, so the next WM_MOUSEMOVE would otherwise be a
      // spurious delta (and the scale may have changed too). Re-establish the
      // relative base on the next move.
      d->mouse_valid = 0;
      d->rel_acc_x = 0;
      d->rel_acc_y = 0;
      InvalidateRect(hwnd, NULL, FALSE);  // the picture scales to the client area
      return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      if (!d->key_cb) return DefWindowProcA(hwnd, msg, wp, lp);  // Alt+F4 etc. stay
      // Ctrl+Alt releases the relative grab (QEMU's convention). Track the
      // modifiers from the key messages themselves: GetKeyState is thread
      // state and can lag a synthetic/queued key event. The first half of the
      // chord may already have reached the guest; the second is consumed so
      // the release itself is not typed into the guest.
      if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (wp == VK_CONTROL) d->ctrl_down = 1;
        if (wp == VK_MENU) d->alt_down = 1;
        if (d->grabbed && d->ctrl_down && d->alt_down) {
          ReleasePointer(d);
          return 0;
        }
      } else {
        if (wp == VK_CONTROL) d->ctrl_down = 0;
        if (wp == VK_MENU) d->alt_down = 0;
      }
      // lp bits 16-23 carry the keyboard's own make code — the set-1 byte the
      // PC/AT sends — and bit 24 marks the 0xe0-prefixed keys (Win32
      // WM_KEYDOWN). The model adds the break bit from `up`.
      uint32_t scan = (uint32_t)((lp >> 16) & 0xff);
      int up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
      d->key_cb(d->key_ctx, scan, (int)((lp >> 24) & 1), up);
      return 0;
    }
    case WM_MOUSEMOVE: {
      int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
      if (d->mouse_abs_cb) {
        // Absolute channel: the picture position, clamped to the nearest edge
        // in the letterbox; nothing at all outside the client area (a captured
        // drag past the edge reports no movement).
        int sx, sy;
        if (MapPointer(d, x, y, &sx, &sy)) {
          d->abs_x = sx;
          d->abs_y = sy;
          d->abs_valid = 1;
          d->mouse_abs_cb(d->mouse_abs_ctx, sx, sy, 0, d->mouse_buttons);
        }
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
      if (!d->mouse_cb) return DefWindowProcA(hwnd, msg, wp, lp);
      if (!d->focused) {
        // Hovering an inactive window must not move the guest cursor.
        d->mouse_x = x;
        d->mouse_y = y;
        d->mouse_valid = 0;
        return 0;
      }
      if (!d->grabbed && d->grab_suppressed) {
        // Released with Ctrl+Alt: keep the host cursor visible and do not
        // move the guest until the user clicks the display again.
        d->mouse_x = x;
        d->mouse_y = y;
        d->mouse_valid = 0;
        return 0;
      }
      if (!d->grabbed) {
        // First movement over a focused relative-only display: grab the
        // pointer (hide the host arrow, recenter, capture). This event only
        // establishes the new base; the next one carries the movement.
        GrabPointer(d);
        return 0;
      }
      // Relative channel (PS/2): Win32 client coordinates grow downward,
      // the PS/2 mouse reports Y positive upward, so the vertical delta is
      // negated. The delta is in source-picture counts: the picture is scaled
      // into the client, so raw client pixels would make the guest cursor
      // faster/slower whenever the window is not 1:1.
      if (d->mouse_valid) {
        int gdx, gdy;
        RelativeDelta(d, x - d->mouse_x, d->mouse_y - y, &gdx, &gdy);
        if (gdx || gdy)
          d->mouse_cb(d->mouse_ctx, gdx, gdy, 0, d->mouse_buttons);
      }
      d->mouse_x = x;
      d->mouse_y = y;
      d->mouse_valid = 1;
      // Keep the hidden host pointer inside the window so the relative
      // movement never runs out; the movement that reached the margin was
      // already reported above, and the warp itself is not a delta.
      if (NearClientEdge(d, x, y)) RecenterPointer(d);
      return 0;
    }
    case WM_MOUSELEAVE:
    case WM_NCMOUSEMOVE:
    case WM_NCMOUSELEAVE:
      // The pointer left the client (including into the title bar or a resize
      // border): the next client move must only establish a new base, or the
      // distance crossed in the non-client area becomes one huge relative
      // delta (a jump to the screen edge).
      d->mouse_valid = 0;
      d->rel_acc_x = 0;
      d->rel_acc_y = 0;
      if (msg == WM_MOUSELEAVE || msg == WM_NCMOUSELEAVE) return 0;
      return DefWindowProcA(hwnd, msg, wp, lp);
    case WM_CAPTURECHANGED:
      // Capture can be lost without a matching button-up (another window takes
      // it). Our own SetCapture/ReleaseCapture pair has lParam == hwnd and is
      // not a loss; anyone else taking it ends the grab and brings the host
      // cursor back.
      if (d->grabbed && (HWND)lp != hwnd) {
        d->grabbed = 0;
        if (d->cursor_hidden) {
          ShowCursor(TRUE);
          d->cursor_hidden = 0;
        }
      }
      d->mouse_valid = 0;
      d->rel_acc_x = 0;
      d->rel_acc_y = 0;
      return 0;
    case WM_KILLFOCUS:
      // Focus going away costs us the pointer too: release the grab, show the
      // host cursor, and make the next WM_MOUSEMOVE re-establish the base.
      d->focused = 0;
      ReleasePointer(d);
      d->ctrl_down = 0;
      d->alt_down = 0;
      d->mouse_valid = 0;
      d->rel_acc_x = 0;
      d->rel_acc_y = 0;
      return DefWindowProcA(hwnd, msg, wp, lp);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
      if (!d->mouse_abs_cb && !d->mouse_cb) return DefWindowProcA(hwnd, msg, wp, lp);
      // Button state after the event: bit 0 left, bit 1 right, bit 2 middle —
      // the order the PS/2 packet's first byte uses.
      int bit = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)     ? 0x01
                : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP)   ? 0x02
                                                                   : 0x04;
      int down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
      // A click into a focused relative-only display starts the grab; the
      // click itself still reaches the guest with the new button state.
      if (down && !d->mouse_abs_cb && d->mouse_cb) {
        d->grab_suppressed = 0;
        if (!d->grabbed) GrabPointer(d);
      }
      if (down)
        d->mouse_buttons |= bit;
      else
        d->mouse_buttons &= ~bit;
      if (d->mouse_abs_cb) {
        // A press/release outside the client (captured drag) still has to
        // reach the guest so a button cannot stick; report it at the last
        // valid picture position, which is the clamped edge.
        int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
        int sx = d->abs_x, sy = d->abs_y;
        if (MapPointer(d, x, y, &sx, &sy)) {
          d->abs_x = sx;
          d->abs_y = sy;
          d->abs_valid = 1;
        }
        if (d->abs_valid) d->mouse_abs_cb(d->mouse_abs_ctx, sx, sy, 0, d->mouse_buttons);
      } else {
        // A press or release with no movement still reaches the guest: the
        // packet carries the buttons, and the device sends one for the event.
        d->mouse_cb(d->mouse_ctx, 0, 0, 0, d->mouse_buttons);
      }
      // Keep reporting while a button is held even if the pointer leaves the
      // client area — that is what a drag is. The relative grab owns capture
      // already and stays grabbed after the button is released (Ctrl+Alt
      // releases it); only the absolute/ungrabbed path captures per drag.
      if (d->mouse_abs_cb || !d->grabbed) {
        if (down)
          SetCapture(hwnd);
        else if (!d->mouse_buttons)
          ReleaseCapture();
      }
      return 0;
    }
    case WM_MOUSEWHEEL: {
      if (!d->mouse_abs_cb && !d->mouse_cb) return DefWindowProcA(hwnd, msg, wp, lp);
      // The wheel arrives in multiples of WHEEL_DELTA; the PS/2 wheel byte
      // counts detents, so one notch is one.
      int dz = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
      if (dz) {
        if (d->mouse_abs_cb) {
          // The wheel's lParam is in screen coordinates; map the real cursor
          // position so the event is both current and inside the client.
          POINT p;
          GetCursorPos(&p);
          ScreenToClient(d->hwnd, &p);
          int sx = d->abs_x, sy = d->abs_y;
          if (MapPointer(d, p.x, p.y, &sx, &sy)) {
            d->abs_x = sx;
            d->abs_y = sy;
            d->abs_valid = 1;
            d->mouse_abs_cb(d->mouse_abs_ctx, sx, sy, dz, d->mouse_buttons);
          }
        } else {
          d->mouse_cb(d->mouse_ctx, 0, 0, dz, d->mouse_buttons);
        }
      }
      return 0;
    }
    case WM_CLOSE:
      ReleasePointer(d);
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      ReleasePointer(d);
      d->closed = 1;
      return 0;
    default:
      return DefWindowProcA(hwnd, msg, wp, lp);
  }
}

// Resizes the window to the picture the device publishes now. The client area
// follows the card's mode: a VGA text mode is 720x400, its EGA graphics mode
// 640x350, its CGA mode 320x200 — one window whose size the mode decides.
static void FollowPictureSize(HostDisplay* d) {
  int w = d->ops->Width(d->dev), h = d->ops->Height(d->dev);
  if (w <= 0 || h <= 0 || (w == d->width && h == d->height)) return;
  d->width = w;
  d->height = h;
  BITMAPINFOHEADER* bh = &d->bi.bmiHeader;
  bh->biWidth = w;
  bh->biHeight = -h;  // top-down rows
  // Keep the window's outer size in step so the client area is exactly one
  // picture again (the user can still resize afterwards: Blit scales).
  RECT rc = {0, 0, w, h};
  AdjustWindowRect(&rc, (DWORD)GetWindowLongPtrA(d->hwnd, GWL_STYLE), FALSE);
  SetWindowPos(d->hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  InvalidateRect(d->hwnd, NULL, FALSE);
}

HostDisplay* HostDisplayOpen(const char* title, const DisplaySourceOps* ops, void* dev) {
  HostDisplay* d = (HostDisplay*)calloc(1, sizeof(*d));
  if (!d) return NULL;
  d->ops = ops;
  d->dev = dev;
  d->width = ops->Width(dev);
  d->height = ops->Height(dev);
  BITMAPINFOHEADER* h = &d->bi.bmiHeader;
  h->biSize = sizeof(*h);
  h->biWidth = d->width;
  h->biHeight = -d->height;  // top-down rows
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

  // Resizable: the emulated card's modes come in several sizes and the user may
  // want a bigger picture; Blit scales the picture to whatever the client area
  // is, preserving its aspect ratio.
  static const DWORD kStyle =
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME;
  RECT rc = {0, 0, d->width, d->height};
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
  // Host time: this throttle exists so the run loop does not reach a syscall on
  // every step, which is a question about how fast the host goes, not about how
  // much emulated time the guest has used.
  int64_t now = HostTimeRealtime();
  if (now < d->next_pump_us) return;
  d->next_pump_us = now + kPumpIntervalUs;
  // Refresh the picture size before dispatching input: the pointer mapping
  // uses the current source dimensions, and a mode change must be visible to
  // the same pump's mouse messages.
  FollowPictureSize(d);
  MSG msg;
  while (PeekMessageA(&msg, d->hwnd, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  if (d->closed) return;
  // Invalidate only: WM_PAINT is the single blit path, so one published frame costs one
  // blit instead of the old immediate blit plus the paint that followed it.
  if (d->ops->Version(d->dev) != d->shown_version) InvalidateRect(d->hwnd, NULL, FALSE);
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

void HostDisplaySetMouseAbsSink(HostDisplay* d,
                                void (*cb)(void* ctx, int x, int y, int dz, int buttons),
                                void* ctx) {
  d->mouse_abs_cb = cb;
  d->mouse_abs_ctx = ctx;
}

void HostDisplayFree(HostDisplay* d) {
  if (!d) return;
  ReleasePointer(d);
  if (d->hwnd) DestroyWindow(d->hwnd);
  if (d->back_dc) DeleteDC(d->back_dc);
  if (d->back_bmp) DeleteObject(d->back_bmp);
  free(d);
}
