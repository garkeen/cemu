#!/usr/bin/env bash
# Builds the S-mode extras (rv64si, rv64mzicbo, rv64ssvnapot) that were not
# part of the original stage-1 xpack-gcc batch. Toolchain: LLVM clang
# (riscv64 cross; the xpack gcc left the tool whitelist — reference.md 三).
#
# Two build adaptations vs the reference riscv-tests Makefile (GNU ld is not
# in the tool set; ld.lld is stricter):
#   1. link.ld: PHDRS FLAGS(SHF_ALLOC | SHF_EXECINSTR) -> FLAGS(0x7); lld has
#      no SHF_* symbols in linker-script scope. 0x7 = R|W|X matches the GNU
#      output's PT_LOAD flags.
#   2. The tests' `.global stvec_handler/mtvec_handler` upgrade a `.weak`
#      declaration (env/p/riscv_test.h) to global — GNU ld tolerates the
#      binding change, lld does not. The .S copies rewrite the lines to
#      `.weak` (same effective binding: the handler is defined in the test).
set -eu
R=${RISCV_TESTS:-D:/code/c/TinyEMU}
OUT=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
sed 's/FLAGS(SHF_ALLOC | SHF_EXECINSTR)/FLAGS(0x7)/' \
  "$R/riscv-test-env/p/link.ld" > "$TMP/link.ld"
build() { # build <suite> <name> <march>
  local src="$R/riscv-tests/isa/$1/$2.S"
  sed 's/^  .global stvec_handler/  .weak stvec_handler/;
       s/^  .global mtvec_handler/  .weak mtvec_handler/' "$src" > "$TMP/$2.S"
  clang --target=riscv64-unknown-elf -march="$3" -mabi=lp64d \
    -static -mcmodel=medany -fvisibility=hidden -nostdlib -nostartfiles \
    -I"$R/riscv-test-env/p" -I"$R/riscv-tests/isa/macros/scalar" \
    -I"$R/riscv-tests/isa" -T "$TMP/link.ld" "$TMP/$2.S" -o "$OUT/rv64$1-p-$2.elf"
}
build si scall rv64g; build si sbreak rv64g; build si csr rv64g
build si dirty rv64g; build si wfi rv64g; build si ma_fetch rv64g
build si icache-alias rv64g
build mzicbo zero rv64g_zicboz
build ssvnapot napot rv64g
rm -rf "$TMP"
