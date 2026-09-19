// Host keyboard source (階段 4 片 6): whatever the process's stdin is — the
// console the user runs cemu from, or a pipe a script drives — becomes
// make/break scan code pairs at the machine's key sink. The PC's guest input

#include "debug/debug.h"
// device is the 8042 keyboard, so both the display window and stdin end up on
// the same wire (IRQ1); typing in the terminal is the fallback when the window
// does not hold the keyboard focus.
//
// ASCII in, set 1 out: the table is the US layout's make codes, and an
// upper-case letter rides on the shift key (shift make, key make, key break,
// shift break) so the guest's own decoder sees a normal shifted keypress.
#include <windows.h>

#include <conio.h>
#include <stdio.h>

#include "host/host.h"

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

static void (*g_sink)(void* ctx, uint32_t scan, int extended, int up);
static void* g_ctx;
static HANDLE g_in;
static int g_in_is_console;
static int g_saw_cr;  // a pipe sends "\r\n": the LF after a CR is not a key

static void Emit(uint8_t scan) {
  g_sink(g_ctx, scan, 0, 0);
  g_sink(g_ctx, (uint32_t)(scan | kScanBreakBit), 0, 1);
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
  if (shift) g_sink(g_ctx, kScanShiftMake, 0, 0);
  Emit(scan);
  if (shift) g_sink(g_ctx, kScanShiftBreak, 0, 1);
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

void HostKeyOpen(void (*sink)(void* ctx, uint32_t scan, int extended, int up), void* ctx) {
  g_sink = sink;
  g_ctx = ctx;
  g_in = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  g_in_is_console = g_in && g_in != INVALID_HANDLE_VALUE && GetConsoleMode(g_in, &mode);
  if (!g_in_is_console && (g_in == NULL || g_in == INVALID_HANDLE_VALUE)) g_sink = NULL;
  g_saw_cr = 0;
  if (DebugOn(kDbgMark)) DebugMark("keyopen", g_sink ? 1 : 0, g_in_is_console);
}

void HostKeyPoll(void) {
  static int announced;
  if (!announced) {
    announced = 1;
    // Self-report at the first poll: this runs after DebugInit, so the note
    // actually prints (an attach-time note would be swallowed), and it fires
    // even when no sink is attached — which is what the keyboard hunt needed.
    if (DebugOn(kDbgMark))
      DebugMark("keypoll", (g_sink != NULL) * 10 + (int)(uintptr_t)g_in, g_in_is_console);
  }
  if (!g_sink) return;
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
    SendChar((unsigned char)c);
  }
}
