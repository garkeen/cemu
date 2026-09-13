#!/usr/bin/env bash
# Builds the kvm-unit-tests 32-bit flat images from the v86 checkout
# (D:/code/c/TinyEMU/v86/tests/kvm-unit-tests) with LLVM clang + ld.lld.
# The images boot through the multiboot header in x86/cstart.S (loader:
# flat protected mode, EAX = 0x2badb002, EBX = multiboot info) and exit via
# the 0xF4 debug-exit device — the same contracts cemu's x86 machine
# implements. Output goes next to this script and is picked up by run.sh.
#
# Harness reality on this machine (QEMU-permissive semantics the tests
# rely on, mirrored in cemu): cstart's setup_percpu_area WRMSRs the
# long-mode GS base (no effect in 32-bit), and enable_apic/smp_init touch
# the LAPIC register page and fw_cfg — both devices exist on the machine.
#
# Build adaptations (lld/IAS are stricter than GNU ld/as; same class as the
# rv64si linker adaptations):
#   1. lib/x86/desc.c: clang's integrated assembler rejects a
#      segment-override mov without a size suffix — the copied source gives
#      the %gs:6 fault-code read an explicit movzwl width.
#   2. lib/x86/desc.c: exception_table_start/end are linker-script symbols
#      placed at the SAME address (an empty table). As C objects, the
#      compiler may assume &start != &end and drop the loop's entry check
#      (clang does) — the scan then runs through unmapped memory. Declaring
#      them as arrays (the canonical fix for linker-script symbol pairs)
#      restores the address-comparison semantics.
#   3. lib objects carry path-mangled names: lib/stack.c and
#      lib/x86/stack.c would otherwise collide in the archive.
set -eu
K=${KVM_UT:-D:/code/c/TinyEMU/v86/tests/kvm-unit-tests}
OUT=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

sed -e 's/asm("mov %%gs:6, %0" : "=rm"(error_code));/asm("movzwl %%gs:6, %k0" : "=r"(error_code));/' \
    -e 's/extern struct ex_record exception_table_start, exception_table_end;/extern struct ex_record exception_table_start[];\nextern struct ex_record exception_table_end[];/' \
    -e 's/for (ex = &exception_table_start; ex != &exception_table_end; ++ex)/for (ex = exception_table_start; ex != exception_table_end; ++ex)/' \
  "$K/lib/x86/desc.c" > "$TMP/desc.c"

CFLAGS="--target=i386-unknown-elf -march=i686 -m32 -static -nostdlib -nostartfiles
  -ffreestanding -fno-stack-protector -fno-pic -fno-pie -O2 -g0
  -I$K/include -I$K/lib -I$K/lib/x86 -I$K/x86"

cobj() { # cobj <src..c> — compiles into $TMP
  clang --target=i386-unknown-elf -march=i686 -m32 -static -nostdlib -nostartfiles \
    -ffreestanding -fno-stack-protector -fno-pic -fno-pie -O2 -g0 -c \
    -I$K/include -I$K/lib -I$K/lib/x86 -I$K/x86 "$K/$1" \
    -o "$TMP/$(echo "${1%.c}" | tr '/' '_').o"
}

for f in lib/argv.c lib/printf.c lib/string.c lib/abort.c lib/report.c lib/stack.c \
         lib/x86/io.c lib/x86/smp.c lib/x86/vm.c \
         lib/x86/fwcfg.c lib/x86/apic.c lib/x86/atomic.c lib/x86/isr.c lib/x86/stack.c \
         lib/x86/setup.c; do
  cobj "$f"
done
clang --target=i386-unknown-elf -march=i686 -m32 -static -nostdlib -nostartfiles \
  -ffreestanding -fno-stack-protector -fno-pic -O1 \
  -I$K/include -I$K/lib -I$K/lib/x86 -I$K/x86 -c "$TMP/desc.c" -o "$TMP/desc.o"
clang --target=i386-unknown-elf -march=i686 -m32 -nostdlib -nostartfiles \
  -fno-stack-protector -fno-pic -O2 -g0 -I$K/include -I$K/lib -I$K/lib/x86 -I$K/x86 \
  -c "$K/lib/x86/setjmp32.S" -o "$TMP/setjmp32.o"
clang --target=i386-unknown-elf -march=i686 -m32 -nostdlib -nostartfiles \
  -fno-stack-protector -fno-pic -O2 -g0 -I$K/include -I$K/lib -I$K/lib/x86 -I$K/x86 \
  -c "$K/x86/cstart.S" -o "$TMP/cstart.o"

rm -f "$TMP/libcflat.a"
llvm-ar rcs "$TMP/libcflat.a" \
  $(find "$TMP" -name '*.o' ! -name 'cstart.o' | sort)
BUILTINS=$TMP/rt_builtins.o

# The 64-bit division helpers are hand-rolled (no ELF i386 compiler-rt in
# this LLVM install) — verify them against the host's / and % on edge cases
# and a deterministic pseudo-random sample before trusting the build.
cat > "$TMP/rt_check.c" <<'EOF'
#include <stdio.h>
typedef unsigned long long u64;
typedef long long s64;
u64 __udivmoddi4(u64, u64, u64*);
u64 __udivdi3(u64, u64);
u64 __umoddi3(u64, u64);
s64 __divdi3(s64, s64);
s64 __moddi3(s64, s64);
static u64 seed = 0x123456789abcdefULL;
static u64 nxt(void) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return seed; }
int main(void) {
  u64 ns[] = {0, 1, 2, 3, ~(u64)0, ~0ULL >> 1, 0x8000000000000000ULL, 0x123456789abcdefULL};
  u64 ds[] = {1, 2, 3, 7, 0x100000000ULL, ~0ULL, 0x123456789abcdefULL};
  for (unsigned i = 0; i < sizeof ns / sizeof *ns; i++)
    for (unsigned j = 0; j < sizeof ds / sizeof *ds; j++) {
      u64 n = ns[i], d = ds[j];
      if (__udivdi3(n, d) != n / d || __umoddi3(n, d) != n % d) return 1;
      s64 sn = (s64)n, sd = (s64)d;
      // INT64_MIN / -1 overflows the host's signed division (hardware trap)
      // — the truncating semantics our helper implements are still checked
      // by every other pair.
      if (sn && sd && !(sn == (-0x7fffffffffffffffLL - 1) && sd == -1)) {
        if (__divdi3(sn, sd) != sn / sd || __moddi3(sn, sd) != sn % sd) return 2;
      }
    }
  for (int k = 0; k < 10000; k++) {
    u64 n = nxt(), d = nxt();
    if (!d) continue;
    u64 r;
    if (__udivmoddi4(n, d, &r) != n / d || r != n % d) return 3;
  }
  printf("rt_builtins ok\n");
  return 0;
}
EOF
gcc "$TMP/rt_check.c" "$OUT/rt_builtins.c" -o "$TMP/rt_check"
"$TMP/rt_check" || {
  echo "rt_builtins host check FAILED (code $?)"
  exit 1
}
clang --target=i386-unknown-elf -march=i686 -m32 -static -nostdlib -nostartfiles \
  -ffreestanding -fno-stack-protector -fno-pic -O2 -g0 -c "$OUT/rt_builtins.c" \
  -o "$TMP/rt_builtins.o"

testimg() { # testimg <name>
  clang --target=i386-unknown-elf -march=i686 -m32 -static -nostdlib -nostartfiles \
    -ffreestanding -fno-stack-protector -fno-pic -O2 -g0 \
    -I$K/include -I$K/lib -I$K/lib/x86 -I$K/x86 -c "$K/x86/$1.c" -o "$TMP/$1.o"
  ld.lld -T "$K/x86/flat.lds" --build-id=none -o "$OUT/$1.elf" \
    "$TMP/cstart.o" "$TMP/$1.o" "$TMP/libcflat.a" "$BUILTINS"
}

testimg_src() { # testimg_src <name> <src> — for the in-tree ports
  clang --target=i386-unknown-elf -march=i686 -m32 -static -nostdlib -nostartfiles \
    -ffreestanding -fno-stack-protector -fno-pic -O2 -g0 \
    -I$K/include -I$K/lib -I$K/lib/x86 -I$K/x86 -c "$2" -o "$TMP/$1.o"
  ld.lld -T "$K/x86/flat.lds" --build-id=none -o "$OUT/$1.elf" \
    "$TMP/cstart.o" "$TMP/$1.o" "$TMP/libcflat.a" "$BUILTINS"
}

testimg taskswitch
testimg taskswitch2
testimg cmpxchg8b
testimg memory
testimg_src debug "$OUT/kut_debug.c"
echo "built: taskswitch taskswitch2 cmpxchg8b memory debug"
