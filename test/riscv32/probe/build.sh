#!/usr/bin/env bash
# Builds probe_rv32.elf (see probe_rv32.S). Same lld adaptations as
# ../build.sh: numeric PHDRS flags in the riscv-test-env link script.
# Zcf/Zcd select the compressed single/double memory forms the ratified C
# extension split out of C (RV32C's c.flw/c.fsw and the shared c.fld/c.fsd).
set -eu
R=${RISCV_TESTS:-D:/code/c/EMU}
OUT=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
sed 's/FLAGS(SHF_ALLOC | SHF_EXECINSTR)/FLAGS(0x7)/' \
  "$R/riscv-test-env/p/link.ld" > "$TMP/link.ld"
clang --target=riscv32-unknown-elf -march=rv32g_zcf_zcd -mabi=ilp32 \
  -static -mcmodel=medany -fvisibility=hidden -nostdlib -nostartfiles \
  -I"$R/riscv-test-env/p" -T "$TMP/link.ld" "$OUT/probe_rv32.S" -o "$OUT/probe_rv32.elf"
rm -rf "$TMP"
