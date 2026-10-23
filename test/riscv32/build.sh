#!/usr/bin/env bash
# Builds the riscv-tests rv32 images committed in this directory. Toolchain:
# LLVM clang (riscv32 cross; the xpack gcc left the tool whitelist —
# reference.md 三). The tests are the official riscv-tests p suite; the
# images are checked in so run.sh needs no toolchain.
#
# Build adaptations vs the reference riscv-tests Makefile (GNU ld is not in
# the tool set; ld.lld is stricter):
#   1. link.ld: PHDRS FLAGS(SHF_ALLOC | SHF_EXECINSTR) -> FLAGS(0x7); lld has
#      no SHF_* symbols in linker-script scope. 0x7 = R|W|X matches the GNU
#      output's PT_LOAD flags.
#   2. The rv32 tree is a wrapper layer: isa/rv32<suite>/<test>.S sets
#      RVTEST_RV32* and #includes isa/rv64<suite>/<test>.S (bodies sometimes
#      include across suites). The whole rv32*/rv64* source tree is mirrored
#      into $TMP so those relative includes still resolve, and every
#      `.global stvec_handler/mtvec_handler` (an upgrade of the weak
#      declaration in env/p/riscv_test.h) is rewritten to `.weak`: GNU ld
#      tolerates the binding change, lld does not. The effective binding is
#      unchanged because the handler is defined in the test.
#   3. Names follow the upstream Makefrags (<suite> <test> ->
#      rv32<suite>-p-<test>.elf).
#   4. isa/rv32ud/move.S is not an RV32 test in this checkout: its rv64ud
#      body emits fmv.d.x / fmv.x.d (RV64-only). It is excluded rather than
#      patched, since the test as written cannot assemble for RV32.
set -eu
R=${RISCV_TESTS:-D:/code/c/EMU}
OUT=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
sed 's/FLAGS(SHF_ALLOC | SHF_EXECINSTR)/FLAGS(0x7)/' \
  "$R/riscv-test-env/p/link.ld" > "$TMP/link.ld"
mkdir -p "$TMP/isa"
cp -r "$R"/riscv-tests/isa/rv32* "$R"/riscv-tests/isa/rv64* "$TMP/isa/"
find "$TMP/isa" -name '*.S' -exec sed -i \
  's/^[[:space:]]*\.global stvec_handler/  .weak stvec_handler/;
   s/^[[:space:]]*\.global mtvec_handler/  .weak mtvec_handler/' {} +
build() { # build <suite> <test>
  local suite=$1 name=$2
  clang --target=riscv32-unknown-elf -march=rv32g -mabi=ilp32 \
    -static -mcmodel=medany -fvisibility=hidden -nostdlib -nostartfiles \
    -I"$R/riscv-test-env/p" -I"$R/riscv-tests/isa/macros/scalar" \
    -I"$R/riscv-tests/isa" -T "$TMP/link.ld" "$TMP/isa/rv32$suite/$name.S" \
    -o "$OUT/rv32$suite-p-$name.elf"
}
for suite in ui um ua uc uf ud mi si; do
  for src in "$R"/riscv-tests/isa/rv32$suite/*.S; do
    name=$(basename "$src" .S)
    [ "$suite/$name" = "ud/move" ] && continue
    build "$suite" "$name"
  done
done
rm -rf "$TMP"
