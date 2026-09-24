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

void HostWriteOut(const char* buf, size_t n);
void HostWriteErr(const char* buf, size_t n);

// Monotonic host clock in microseconds since an arbitrary epoch (QPC). This is
// the time base every device's period is measured against (PIT counters, APIC
// timer, CLINT mtime).
int64_t HostTimerNow(void);
// Yields the CPU; used while a guest sleeps waiting for a timer interrupt.
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
// Shows a fixed XRGB (0xffRRGGBB) pixel buffer owned by a device; the window
// repaints whenever version_cb(dev) returns a counter different from the one
// last shown. The pump (HostDisplayPump) is called from the board run loop —
// no extra thread, no guest-facing API.
typedef struct HostDisplay HostDisplay;

HostDisplay* HostDisplayOpen(const char* title, int width, int height,
                             const uint32_t* fb,
                             uint32_t (*version_cb)(void* dev), void* dev);
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
// counts, because that is what the PS/2 packet carries; `dz` is the wheel in
// detents and `buttons` the state after the event — bit 0 left, bit 1 right,
// bit 2 middle, the order the packet's first byte uses.
void HostDisplaySetMouseSink(HostDisplay* d,
                             void (*cb)(void* ctx, int dx, int dy, int dz, int buttons),
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

#endif
