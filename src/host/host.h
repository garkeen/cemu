#ifndef CEMU_HOST_HOST_H
#define CEMU_HOST_HOST_H

#include <stddef.h>
#include <stdint.h>

typedef struct HostFile HostFile;

HostFile* HostFileOpenRead(const char* path);
int64_t HostFileSize(HostFile* f);
size_t HostFileRead(HostFile* f, void* buf, size_t n);
void HostFileClose(HostFile* f);

void HostWriteOut(const char* buf, size_t n);
void HostWriteErr(const char* buf, size_t n);

// Monotonic host clock in microseconds since an arbitrary epoch (QPC).
int64_t HostTimerNow(void);
// Yields the CPU; used while a guest sleeps waiting for a timer interrupt.
void HostSleepMs(int ms);

// ---- TCP sockets (gdb stub transport; winsock lives only in host/) ----------
typedef struct HostSock HostSock;

// Listens on 0.0.0.0:port (the QEMU `-gdb tcp::port` contract); NULL on error.
HostSock* HostSockListen(int port);
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
// 1 once the user closed the window (the caller ends the emulation).
int HostDisplayClosed(const HostDisplay* d);
void HostDisplayFree(HostDisplay* d);

#endif
