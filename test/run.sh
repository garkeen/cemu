# Runs every riscv-tests image through cemu; a test passes when the exit
# code is 0 (HTIF exit code, pass writes 1 -> exit 0).
CEMU=${CEMU:-build/cemu.exe}
pass=0
fail=0
for f in "$(dirname "$0")"/*.elf; do
  if "$CEMU" "$f" >/dev/null 2>&1; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "FAIL ($(basename "$f"))"
  fi
done
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
