#ifndef CEMU_HOST_HOST_H
#define CEMU_HOST_HOST_H

#include <stddef.h>
#include <stdint.h>

typedef struct HostFile HostFile;

HostFile* HostFileOpenRead(const char* path);
// Read-write image file: a disk image has sectors the guest writes back.
HostFile* HostFileOpenReadWrite(const char* path);
// Truncating create for files this process produces rather than consumes
// (debug dumps): the old contents are never wanted and a partial rewrite
// would leave a stale tail.
HostFile* HostFileCreate(const char* path);
int64_t HostFileSize(HostFile* f);
size_t HostFileRead(HostFile* f, void* buf, size_t n);
// Positioned access. A disk is addressed by sector number, not by a stream
// cursor, so a transfer carries its own offset.
size_t HostFileReadAt(HostFile* f, int64_t off, void* buf, size_t n);
size_t HostFileWriteAt(HostFile* f, int64_t off, const void* buf, size_t n);
void HostFileClose(HostFile* f);
// Deletes a path (the pclink job-file protocol unlinks its job files; a
// missing path is an error like unlink(2)'s). 0 = removed.
int HostFileRemove(const char* path);

void HostWriteOut(const char* buf, size_t n);
void HostWriteErr(const char* buf, size_t n);

// The emulated clock, in microseconds: the time base every device's period is
// measured against (PIT counters, APIC timer, CLINT mtime, the VGA retrace).
// Host wall time plus any --skip-idle warp, unless -icount replaced the wall
// clock with a declared per-instruction rate (HostTimerIcountEnable).
int64_t HostTimerNow(void);
// The host's own wall clock, for the three things that measure host time rather
// than emulated time: the MC146818 time-of-day, the window's repaint throttle,
// the input poll throttle. Neither -icount nor --skip-idle may move it — the
// MC146818 keeps real time while the guest does not, and a host-side throttle
// that skipped ahead would stop throttling (QEMU's QEMU_CLOCK_REALTIME, which
// hw/rtc/mc146818.c uses for the time-of-day registers).
int64_t HostTimeRealtime(void);
// -icount shift=N (QEMU's spelling): declare that one retired instruction
// accounts for 1<<shift nanoseconds of emulated time, so the emulated clock is
// a function of the executed instruction stream and a run reproduces exactly
// instead of depending on how fast the host happened to be. Enabled before the
// machine runs; the run loop calls Advance() once per retired instruction.
// Halting therefore costs no emulated time, so a halted CPU must be jumped to
// the next device deadline rather than waited out (QEMU advances its icount
// deadline across a halt for the same reason).
void HostTimerIcountEnable(int shift);
// 1 while the emulated clock is instruction-driven rather than wall-driven.
int HostTimerIcountOn(void);
// Charges one retired instruction to the emulated clock; a no-op unless
// -icount is on.
void HostTimerIcountAdvance(void);
// Yields the CPU; used while a guest sleeps waiting for a timer interrupt.
// Under -icount the wait is also charged to the emulated clock, since a halt
// that advances nothing would leave the guest with a clock that never moves.
void HostSleepMs(int ms);
// Advances the clock HostTimerNow() reports by us microseconds, i.e. makes
// emulated time jump forward without host time passing. The run loop uses it
// while the CPU is halted to reach the next device deadline at once instead of
// waiting for it in wall time (--skip-idle; the same idea as QEMU's virtual
// clock warp). Monotonic: a non-positive argument is ignored.
void HostTimerWarp(int64_t us);

// ---- TCP sockets (gdb stub transport; winsock lives only in host/) ----------
typedef struct HostSock HostSock;

// Listens on 0.0.0.0:port (the QEMU `-gdb tcp::port` contract); NULL on error.
HostSock* HostSockListen(int port);
// Connects to host:port (the cemugui front-end is the stub's client); NULL on error.
HostSock* HostSockConnect(const char* host, int port);
// Blocks until a client connects; NULL on error.
HostSock* HostSockAccept(HostSock* l);
// Non-blocking accept; NULL when no client is waiting.
HostSock* HostSockAcceptPoll(HostSock* l);
// Reads up to cap bytes; returns bytes read, 0 = closed/error.
int HostSockRead(HostSock* s, void* buf, int cap);
// Writes exactly n bytes; returns n, 0 = closed/error.
int HostSockWrite(HostSock* s, const void* buf, int n);
// 1 when data is waiting (non-blocking), else 0.
int HostSockReadable(HostSock* s);
void HostSockClose(HostSock* s);

// ---- Display window (display_win.c; GDI, same thread as the run loop) ------
// Shows a device picture through the DisplaySourceOps vtable (device/video/display.h):
// the window reads the size, the XRGB pixel buffer and a change counter from
// the device, so a card whose mode changes size (the VGA family core) is
// followed — the window resizes itself to the mode and scales the picture into
// the client area otherwise. The pump (HostDisplayPump) is called from the
// board run loop — no extra thread, no guest-facing API.
typedef struct HostDisplay HostDisplay;
// The display-source vtable (device/video/display.h) is named here as an
// incomplete type: the window calls it, the host layer does not define it.
typedef struct DisplaySourceOps DisplaySourceOps;

HostDisplay* HostDisplayOpen(const char* title, const DisplaySourceOps* ops, void* dev);
// Message pump + repaint; cheap enough to call every run-loop step (it
// rate-limits itself internally).
void HostDisplayPump(HostDisplay* d);

// The keyboard channel: keys the window receives go to the machine's input
// sink (on the PC that is the 8042 keyboard controller). `scan` is the
// hardware scan code the host reports — the same set-1 make code the PC/AT
// keyboard sends — with `extended` set for the 0xe0-prefixed keys and `up`
// for a release.
void HostDisplaySetKeySink(HostDisplay* d,
                           void (*cb)(void* ctx, uint32_t scan, int extended, int up),
                           void* ctx);

// The pointer channel: the window's mouse messages go to the machine's pointer
// sink (on the PC that is the 8042's auxiliary port and its PS/2 mouse). The
// window reports movement as a delta from the previous position, in mouse
// counts (dx to the right, dy up — the PS/2 Y axis, so a backend whose screen
// coordinates grow downward negates its vertical delta), because that is what
// the PS/2 packet carries; `dz` is the wheel in detents and `buttons` the state
// after the event — bit 0 left, bit 1 right, bit 2 middle, the order the
// packet's first byte uses.
void HostDisplaySetMouseSink(HostDisplay* d,
                             void (*cb)(void* ctx, int dx, int dy, int dz, int buttons),
                             void* ctx);

// The absolute pointer channel (all boards): source-picture pixel coordinates
// (0..Width-1, 0..Height-1, Y down). While the pointer is inside the client
// area the window reports the position mapped to the picture, clamped to the
// nearest picture edge when it is in the letterbox; a point outside the client
// area reports nothing at all. The picture rect is recomputed on every event
// from the current client size and the source size, so window resizing needs
// no cached geometry. A board with a relative-only device (PS/2) translates
// this into its own coordinate space; a board with an absolute device
// (virtio-tablet) sends it as-is. When both sinks are installed the window
// prefers this one; mouse_in stays available for the debug hub's synthetic
// events.
void HostDisplaySetMouseAbsSink(HostDisplay* d,
                                void (*cb)(void* ctx, int x, int y, int dz, int buttons),
                                void* ctx);

// The host's own keyboard source: stdin, read at the machine's input sinks —
// the console the user runs cemu from, or a pipe a script drives. A board with
// a serial port takes stdin at its receiver (raw bytes); otherwise it takes it
// at the keyboard sink as scan codes. HostInputPoll is called from the run loop
// and self-gates to a few milliseconds between OS polls.
void HostInputOpen(void (*serial)(void* ctx, int ch), void* serial_ctx,
                   void (*key)(void* ctx, uint32_t scan, int extended, int up), void* key_ctx);
void HostInputPoll();
// 1 once the user closed the window (the caller ends the emulation).
int HostDisplayClosed(const HostDisplay* d);
void HostDisplayFree(HostDisplay* d);

// ---- system clipboard (clipboard_win.c) ---------------------------------------
// UTF-8 text in and out, the SDL_GetClipboardText / SDL_SetClipboardText
// contract. HostClipboardGetText returns a malloc'd string (caller frees) or
// NULL when there is no text; HostClipboardSetText returns 0 on delivery.
char* HostClipboardGetText(void);
int HostClipboardSetText(const char* text);

#endif
