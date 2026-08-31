# x86_min suites: the boot-sector smoke image and the kvm-unit-tests realmode
# suite, both calibrated against
#   qemu-system-i386 -device isa-debug-exit,iobase=0xf4,iosize=0x4
# (exit status = (value<<1)|1, not value+1; QEMU realmode reference output
# was byte-identical to cemu's first 69 lines and reports 110 PASS total).
dir=$(cd "$(dirname "$0")" && pwd)
CEMU=${CEMU:-$dir/../../build/cemu.exe}
pass=0
fail=0

# Smoke: boot sector prints "cemu-x86-smoke" on COM1 (the QEMU dual-run golden
# output) and exits via debug-exit with value 5 -> status 11.
out=$("$CEMU" --machine x86 --isa x86 "$dir/smoke/x86_smoke.bin" 2>&1)
rc=$?
if [ "$rc" -eq 11 ] && echo "$out" | grep -q cemu-x86-smoke; then
  pass=$((pass + 1))
else
  fail=$((fail + 1))
  echo "FAIL (smoke: rc=$rc)"
fi

# Realmode: suite self-reports PASS/FAIL lines. Today it halts at
# test_sti_inhibit's hlt (needs PIT+PIC interrupt wake, stage 2), so the
# reachable count is 69; raise it when stage 2 lands (QEMU: 110).
expected_pass=69
out=$("$CEMU" --machine x86 --isa x86 "$dir/realmode/realmode.elf" 2>&1)
# Note: cemu's [info] loader line has no trailing newline, gluing the first
# "PASS:" to it, so match PASS:/FAIL: anywhere in a line, not at line start.
n_pass=$(echo "$out" | grep -c 'PASS:')
n_fail=$(echo "$out" | grep -c 'FAIL:')
if [ "$n_fail" -eq 0 ] && [ "$n_pass" -eq "$expected_pass" ]; then
  pass=$((pass + 1))
else
  fail=$((fail + 1))
  echo "FAIL (realmode: $n_pass pass, $n_fail fail)"
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
