#!/usr/bin/env bash
# Builds the v86 tests/nasm images that test/x86/run.sh runs (the 16-bit and
# 32-bit divide-error / invalid-opcode cases). The sources stay in the v86
# checkout — same arrangement as build_kut.sh — and only the built ELF lands
# next to this script, so a machine without nasm/ld.lld can still run the
# suite from the checked-in images.
#
#   bash build.sh            # from this directory
#
# V86_UT overrides the checkout path (build_kut.sh has the same knob).
set -eu
N=${V86_UT:-D:/code/c/EMU/v86}/tests/nasm
OUT=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# header.inc is the suite's own (it zeroes the GP registers, sets ESP to
# stack_top and clears EFLAGS); footer.inc is the hlt loop. Both are picked up
# through -I$N, so the v86 tree is never modified.
build() { # build <stem>
  nasm -w+error -f elf32 -I"$N" -o "$TMP/$1.o" "$N/$1.asm"
  ld.lld -m elf_i386 -T "$OUT/mb.ld" -o "$OUT/$1.elf" "$TMP/$1.o"
  echo "built: $1"
}

build idiv8-overflow
build idiv16-overflow
build idiv32-overflow
build mov_sreg_ud1
build mov_sreg_ud2
