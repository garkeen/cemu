// The 64-bit division helpers the i386 freestanding build links against
// (i386 has no 64÷64 hardware division; this LLVM install ships no ELF
// i386 compiler-rt builtins). Standard shift-subtract restoring division.
// test/x86/build_kut.sh verifies this file against the host's / and %
// before trusting it.
typedef unsigned long long u64;
typedef long long s64;

u64 __udivmoddi4(u64 n, u64 d, u64* rem) {
  u64 q = 0, r = 0;
  int i;
  if (!d) return 0 / (u64)d;  // the hardware's own division-by-zero trap
  for (i = 63; i >= 0; i--) {
    r = (r << 1) | ((n >> i) & 1);
    if (r >= d) {
      r -= d;
      q |= 1ULL << i;
    }
  }
  if (rem) *rem = r;
  return q;
}

u64 __udivdi3(u64 n, u64 d) { return __udivmoddi4(n, d, 0); }
u64 __umoddi3(u64 n, u64 d) {
  u64 r;
  __udivmoddi4(n, d, &r);
  return r;
}

s64 __divdi3(s64 n, s64 d) {
  int neg = (n < 0) != (d < 0);
  u64 un = n < 0 ? -(u64)n : (u64)n;
  u64 ud = d < 0 ? -(u64)d : (u64)d;
  u64 q = __udivmoddi4(un, ud, 0);
  return neg ? -(s64)q : (s64)q;
}

s64 __moddi3(s64 n, s64 d) {
  u64 un = n < 0 ? -(u64)n : (u64)n;
  u64 ud = d < 0 ? -(u64)d : (u64)d;
  u64 r;
  __udivmoddi4(un, ud, &r);
  return n < 0 ? -(s64)r : (s64)r;
}
