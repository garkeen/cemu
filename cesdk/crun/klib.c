// klib: the freestanding primitives every stage-5 program needs.  Compiled
// with -fno-builtin so the loops are not turned back into calls to themselves.

#include "cesdk.h"

void *Memcpy(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  while (n--) *d++ = *s++;
  return dst;
}

void *Memmove(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  if (d <= s || d >= s + n) return Memcpy(dst, src, n);
  d += n;
  s += n;
  while (n--) *--d = *--s;
  return dst;
}

void *Memset(void *dst, int c, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  while (n--) *d++ = (uint8_t)c;
  return dst;
}

int Memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *x = (const uint8_t *)a;
  const uint8_t *y = (const uint8_t *)b;
  for (size_t i = 0; i < n; i++) {
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  }
  return 0;
}

size_t Strlen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

int Strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

void Puts(const char *s) {
  while (*s) Putch(*s++);
}

void Puthex(uint64_t v) {
  static const char kDigits[] = "0123456789abcdef";
  char buf[16];
  int n = 0;
  do {
    buf[n++] = kDigits[v & 0xf];
    v >>= 4;
  } while (v);
  while (n--) Putch(buf[n]);
}

void Putdec(uint64_t v) {
  char buf[20];
  int n = 0;
  do {
    buf[n++] = (char)('0' + (v % 10));
    v /= 10;
  } while (v);
  while (n--) Putch(buf[n]);
}
