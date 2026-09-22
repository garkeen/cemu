# Runs every riscv-tests image through cemu on the spike_min machine; a test
# passes when the exit code is 0 (HTIF exit code, pass writes 1 -> exit 0).
#
# The guest's own output is printed with every failure: the exit code alone
# cannot tell "the image would not open" from "the test ran and failed", and
# throwing that output away makes a failing run undiagnosable (the same gap the
# x86 suite had, stage 4 片 13).
dir=$(cd "$(dirname "$0")" && pwd)
CEMU=${CEMU:-$dir/../../build/cemu.exe}
pass=0
fail=0
for f in "$dir"/*.elf; do
  out=$(timeout 60 "$CEMU" "$f" 2>&1)
  rc=$?
  if [ "$rc" -eq 0 ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "FAIL ($(basename "$f"): rc=$rc)"
    echo "$out" | tail -n "${FAIL_TAIL:-12}" | sed 's/^/    | /'
  fi
done
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
