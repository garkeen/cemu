#!/usr/bin/env bash
# Dependency-edge check (AGENTS.md 第七节).
#
# The tree is organised by the machine part a module emulates (cpu/, bus/,
# mem/, device/, board/, debug/). The include rules below keep the module
# boundaries from eroding; this script fails the build when one is violated.
set -u
cd "$(dirname "$0")/.." || exit 1

fail=0
forbid() {  # forbid <message> <grep -e pattern> <dir...>
  local msg="$1"; shift
  local out
  out=$(grep -rn "$@" 2>/dev/null)
  if [ -n "$out" ]; then
    echo "$out" | while IFS= read -r line; do echo "dep: $line  <- $msg"; done
    fail=1
  fi
}

# bus, mem, device and debug talk to CpuState (cpu/cpu.h), never to the CPU
# model behind it: this is the "devices do not know the CPU model" rule.
for d in src/bus src/mem src/device src/debug; do
  forbid "a CPU model ($d must not include cpu/isa/)" \
         -e '#include[[:space:]]*"cpu/isa/' "$d"
done

# The cpu/ contract files must not include a model either.
forbid "cpu/ contract files must not include a CPU model" \
       -e '#include[[:space:]]*"cpu/isa/' src/cpu/step.h src/cpu/step.c src/cpu/cpu.h

# An interpreter knows its own architecture; it must not reach into the
# peripherals or the board around it.
forbid "an interpreter must not include device/" \
       -e '#include[[:space:]]*"device/' src/cpu/isa
forbid "an interpreter must not include board/" \
       -e '#include[[:space:]]*"board/' src/cpu/isa

# windows.h is host/ property; everything else stays portable C.
out=$(grep -rn '#include[[:space:]]*<windows\.h>' src --include=*.c --include=*.h 2>/dev/null |
      grep -v '^src/host/')
if [ -n "$out" ]; then
  echo "$out" | while IFS= read -r line; do echo "dep: $line  <- windows.h outside host/"; done
  fail=1
fi

if [ "$fail" -eq 0 ]; then
  echo "dep: ok (no forbidden include edges)"
fi
exit $fail
