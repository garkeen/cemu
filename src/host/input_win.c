// Host input source: whatever the process's stdin is — the console the user
// runs cemu from, or a pipe a script drives — becomes guest input. There is one
// reader and two shapes of the same bytes:
//
//   * the serial receiver, which takes the byte itself (a terminal on COM1 or
//     on the virt machine's ns16550a), and
//   * the keyboard controller, which takes set-1 scan codes (the PC's 8042).
//
// A board that has a serial port takes stdin there and leaves the window's
// keyboard on the PS/2 wire — the QEMU `-serial stdio` model, where the two
// sources stay separate. A board with no serial port takes stdin at the
// keyboard, which is how the PC's console was driven before COM1 had a
// receiver.
//
// ASCII in, set 1 out for the keyboard path: the table is the US layout's make
// codes, and an upper-case letter rides on the shift key (shift make, key make,
// key break, shift break) so the guest's own decoder sees a normal shifted
// keypress.

#include "host/host.h"

#include <windows.h>

#include <conio.h>
#include <stdio.h>

#include "debug/debug.h"

typedef struct KeyMap {
  char ch;
  uint8_t scan;
  int shift;
} KeyMap;

// Set 1 make codes (PC/AT Technical Reference; the same bytes a real keyboard
// sends and the ones xv6's kbd.c decodes).
static const KeyMap kAsciiMap[] = {
    {'a', 0x1e, 0}, {'b', 0x30, 0}, {'c', 0x2e, 0}, {'d', 0x20, 0}, {'e', 0x12, 0},
    {'f', 0x21, 0}, {'g', 0x22, 0}, {'h', 0x23, 0}, {'i', 0x17, 0}, {'j', 0x24, 0},
    {'k', 0x25, 0}, {'l', 0x26, 0}, {'m', 0x32, 0}, {'n', 0x31, 0}, {'o', 0x18, 0},
    {'p', 0x19, 0}, {'q', 0x10, 0}, {'r', 0x13, 0}, {'s', 0x1f, 0}, {'t', 0x14, 0},
    {'u', 0x16, 0}, {'v', 0x2f, 0}, {'w', 0x11, 0}, {'x', 0x2d, 0}, {'y', 0x15, 0},
    {'z', 0x2c, 0}, {'1', 0x02, 0}, {'2', 0x03, 0}, {'3', 0x04, 0}, {'4', 0x05, 0},
    {'5', 0x06, 0}, {'6', 0x07, 0}, {'7', 0x08, 0}, {'8', 0x09, 0}, {'9', 0x0a, 0},
    {'0', 0x0b, 0}, {' ', 0x39, 0}, {'\r', 0x1c, 0}, {'\n', 0x1c, 0}, {'\t', 0x0f, 0},
    {'\b', 0x0e, 0}, {0x1b, 0x01, 0}, {'-', 0x0c, 0}, {'=', 0x0d, 0}, {'[', 0x1a, 0},
    {']', 0x1b, 0}, {'\\', 0x2b, 0}, {';', 0x27, 0}, {'\'', 0x28, 0}, {'`', 0x29, 0},
    {',', 0x33, 0}, {'.', 0x34, 0}, {'/', 0x35, 0},
};

enum { kScanShiftMake = 0x2a, kScanShiftBreak = 0xaa, kScanBreakBit = 0x80 };

static void (*g_serial)(void* ctx, int ch);
static void* g_serial_ctx;
static void (*g_key)(void* ctx, uint32_t scan, int extended, int up);
static void* g_key_ctx;
static HANDLE g_in;
static int g_in_is_console;
static int g_saw_cr;  // a pipe sends "\r\n": the LF after a CR is not a key

static void Emit(uint8_t scan) {
  g_key(g_key_ctx, scan, 0, 0);
  g_key(g_key_ctx, (uint32_t)(scan | kScanBreakBit), 0, 1);
}

static void SendChar(unsigned char ch) {
  int shift = 0;
  uint8_t scan = 0;
  if (ch >= 'A' && ch <= 'Z') {
    ch = (unsigned char)(ch - 'A' + 'a');
    shift = 1;
  }
  for (size_t i = 0; i < sizeof(kAsciiMap) / sizeof(kAsciiMap[0]); i++) {
    if ((unsigned char)kAsciiMap[i].ch == ch) {
      scan = kAsciiMap[i].scan;
      break;
    }
  }
  if (scan == 0) return;  // no scan code for this byte: dropped
  if (shift) g_key(g_key_ctx, kScanShiftMake, 0, 0);
  Emit(scan);
  if (shift) g_key(g_key_ctx, kScanShiftBreak, 0, 1);
}

// Returns the next byte, or -1 when nothing is waiting.
static int NextByte(void) {
  if (g_in_is_console) {
    if (!_kbhit()) return -1;
    int c = _getch();
    if (c == 0 || c == 0xe0) {  // an extended key: eat its second byte
      _getch();
      return -1;
    }
    return c;
  }
  DWORD avail = 0;
  if (!PeekNamedPipe(g_in, NULL, 0, NULL, &avail, NULL) || avail == 0) return -1;
  char c = 0;
  DWORD got = 0;
  if (!ReadFile(g_in, &c, 1, &got, NULL) || got != 1) return -1;
  return (unsigned char)c;
}

void HostInputOpen(void (*serial)(void* ctx, int ch), void* serial_ctx,
                   void (*key)(void* ctx, uint32_t scan, int extended, int up), void* key_ctx) {
  // A board with a serial port takes stdin at the receiver; the window keeps
  // driving the keyboard sink (HostDisplaySetKeySink). The keyboard path from
  // stdin stays available only where there is no serial port to feed.
  g_serial = serial;
  g_serial_ctx = serial_ctx;
  g_key = serial ? NULL : key;
  g_key_ctx = key_ctx;
  g_in = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  g_in_is_console = g_in && g_in != INVALID_HANDLE_VALUE && GetConsoleMode(g_in, &mode);
  if (!g_in_is_console && (g_in == NULL || g_in == INVALID_HANDLE_VALUE)) {
    g_serial = NULL;
    g_key = NULL;
  }
  g_saw_cr = 0;
  if (DebugOn(kDbgMark)) DebugMark("inputopen", g_serial ? 1 : (g_key ? 2 : 0), g_in_is_console);
}

void HostInputPoll(void) {
  static int announced;
  if (!announced) {
    announced = 1;
    // Self-report at the first poll: this runs after DebugInit, so the note
    // actually prints (an attach-time note would be swallowed), and it fires
    // even when no sink is attached — which is what the keyboard hunt needed.
    if (DebugOn(kDbgMark))
      DebugMark("inputpoll", (g_serial ? 1 : 0) * 10 + (g_key ? 1 : 0), g_in_is_console);
  }
  if (!g_serial && !g_key) return;
  // A 2 ms gate: the run loop steps millions of times a second and each poll
  // that reaches the OS costs a syscall.
  static int64_t next_us;
  int64_t now = HostTimerNow();
  if (now < next_us) return;
  next_us = now + 2000;
  for (;;) {
    int c = NextByte();
    if (c < 0) return;
    if (DebugOn(kDbgMark)) DebugMark("hostbyte", c, 0);
    if (g_saw_cr && c == '\n') {  // the LF of a "\r\n" is not a second Enter
      g_saw_cr = 0;
      continue;
    }
    g_saw_cr = c == '\r';
    if (g_serial)
      g_serial(g_serial_ctx, c);
    else
      SendChar((unsigned char)c);
  }
}
