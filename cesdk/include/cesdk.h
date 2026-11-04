#ifndef CESDK_CESDK_H
#define CESDK_CESDK_H

// cesdk: the freestanding runtime shared by the firmware, the kernel and the
// userland of the cemu self-hosting chain (arch.md 阶段 5).  Nothing here
// depends on a host OS: the arch backends talk to the machine the board hands
// over, and the only contract is that `_start` reaches `main` with a valid
// stack.

#include <stddef.h>
#include <stdint.h>

// Console: one byte out, one byte in (blocking read returns -1 when the board
// has no input source).
void Putch(char c);
void Puts(const char *s);
int Getch(void);

// Hex dump helpers used by every stage-5 program until printf exists.
void Puthex(uint64_t v);
void Putdec(uint64_t v);

// End the run.  code 0 = success.  Each arch maps that onto the board's
// stop device (riscv: sifive_test PASS; x86: isa-debug-exit payload 5, the
// convention test/x86 uses).  Never returns.
void Halt(int code) __attribute__((noreturn));

// klib: freestanding memory/string primitives (no libc on the guest).
void *Memcpy(void *dst, const void *src, size_t n);
void *Memmove(void *dst, const void *src, size_t n);
void *Memset(void *dst, int c, size_t n);
int Memcmp(const void *a, const void *b, size_t n);
size_t Strlen(const char *s);
int Strcmp(const char *a, const char *b);

#endif
