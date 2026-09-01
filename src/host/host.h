#ifndef CEMU_HOST_HOST_H
#define CEMU_HOST_HOST_H

#include <stddef.h>
#include <stdint.h>

typedef struct HostFile HostFile;

HostFile *HostFileOpenRead(const char *path);
int64_t HostFileSize(HostFile *f);
size_t HostFileRead(HostFile *f, void *buf, size_t n);
void HostFileClose(HostFile *f);

void HostWriteOut(const char *buf, size_t n);
void HostWriteErr(const char *buf, size_t n);

// Monotonic host clock in microseconds since an arbitrary epoch (QPC).
int64_t HostTimerNow(void);
// Yields the CPU; used while a guest sleeps waiting for a timer interrupt.
void HostSleepMs(int ms);

#endif
