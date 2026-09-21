# x86_min suites: the boot-sector smoke image and the kvm-unit-tests realmode
# suite, both calibrated against
#   qemu-system-i386 -device isa-debug-exit,iobase=0xf4,iosize=0x4
# (exit status = (value<<1)|1, not value+1; QEMU realmode reference output
# was byte-identical to cemu's first 110 lines and reports 110 PASS before
# stalling on its own mid-suite).
dir=$(cd "$(dirname "$0")" && pwd)
CEMU=${CEMU:-$dir/../../build/cemu.exe}
pass=0
fail=0

# Smoke: boot sector prints "cemu-x86-smoke" on COM1 (the QEMU dual-run golden
# output) and exits via debug-exit with value 5 -> status 11.
out=$(timeout 30 "$CEMU" --machine x86 --isa x86 "$dir/smoke/x86_smoke.bin" 2>&1)
rc=$?
if [ "$rc" -eq 11 ] && echo "$out" | grep -q cemu-x86-smoke; then
  pass=$((pass + 1))
else
  fail=$((fail + 1))
  echo "FAIL (smoke: rc=$rc)"
fi

# CGA: the display-card probe (阶段 3.5 片 2). Headless-verified device
# semantics: VRAM write/readback at 0xB8000, MC6845 cursor-address program +
# readback through 0x3D4/0x3D5, and a 0x3DA vertical-retrace edge (status
# bit 3). Calibrated against qemu-system-i386 (same binary, same output,
# same status — QEMU's VGA shares the 6845 register contract). The rendered
# window itself is judged by eye via cga_hello.bin + -display win32.
out=$(timeout 30 "$CEMU" --machine x86 --isa x86 "$dir/cga/cga_probe.bin" 2>&1)
rc=$?
if [ "$rc" -eq 11 ] && echo "$out" | grep -q 'cga-probe ok'; then
  pass=$((pass + 1))
else
  fail=$((fail + 1))
  echo "FAIL (cga: rc=$rc)"
fi

# REP-prefixed string ops with (E)CX = 0 (test/x86/probe/rep_zero.asm): the
# prefix's counter test stands in front of the loop body (SDM vol.2 REP
# prefix), so a zero counter means no operation at all. libc-style memcpy
# copies the dword part with `shr ecx,2; rep movsd` and depends on that for
# 2/3-byte tails; the ldlinux core's memcpy (guest 0x1038e0, called by
# do_sysappend) is that shape, and running one body with a zero counter
# wrapped ECX to 0xffffffff and hung the linux.iso boot (stage 4 片 11).
# Dual run against qemu-system-i386: identical transcript, identical status 11.
repbin="$dir/probe/rep_zero.bin"
if [ ! -f "$repbin" ] && command -v nasm > /dev/null 2>&1; then
  nasm -f bin -o "$repbin" "$dir/probe/rep_zero.asm"
fi
if [ -f "$repbin" ]; then
  out=$(timeout 30 "$CEMU" --machine x86 --isa x86 "$repbin" 2>&1)
  rc=$?
  if [ "$rc" -eq 11 ] && echo "$out" | grep -q 'rep-zero ok'; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "FAIL (rep-zero: rc=$rc)"
  fi
else
  echo "SKIP (rep-zero: no nasm and no prebuilt probe bin)"
fi

# IRQ0 delivery to a spinning guest (test/x86/probe/pit_irq.asm): the probe
# builds an IDT, remaps the 8259 pair, programs 8254 channel 0 for 100 Hz,
# unmasks IRQ0 and then busy-waits (deliberately no hlt) counting ticks. A
# guest that only receives timer edges while halted would spin forever, which
# is how Linux's calibrate_delay_converge() (a jiffies spin) behaves in the
# linux.iso boot. Dual run against qemu-system-i386: both report ticks and
# exit 11.
pitbin="$dir/probe/pit_irq.bin"
if [ ! -f "$pitbin" ] && command -v nasm > /dev/null 2>&1; then
  nasm -f bin -o "$pitbin" "$dir/probe/pit_irq.asm"
fi
if [ -f "$pitbin" ]; then
  out=$(timeout 40 "$CEMU" --machine x86 --isa x86 "$pitbin" 2>&1)
  rc=$?
  if [ "$rc" -eq 11 ] && echo "$out" | grep -q 'pit-irq ok'; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "FAIL (pit-irq: rc=$rc)"
  fi
else
  echo "SKIP (pit-irq: no nasm and no prebuilt probe bin)"
fi

# PM: the protected-mode smoke probe (test/x86/pm). Multiboot ELF enters flat
# PM, rebuilds GDT/IDT/TSS, and walks the stage-3 semantics: descriptor
# loads, limit #GP, same-priv and cross-ring gate delivery with TSS stack
# switch, gate-DPL violation #GP(ec=vec*8|2), IRET privilege round-trips,
# call gates (same-priv, inward with stack-parameter copy), far RETF with the
# SDM double-imm outer return, task switching (lcall/ljmp through a TSS,
# state save/restore round trip, busy-bit #GP, IRET nested-task return,
# IDT task gates incl. error-code delivery), paging (identity map +
# CR0.PG|WP on, not-present #PF ec=2 + CR2, read-only write #PF ec=3 under
# CR0.WP=1, A/D bits on successful translations, ring-3 user/supervisor
# #PF ec=7), and the LDT (LLDT/SLDT round trip, TI=1 data loads with limit
# enforcement, cleared-LDTR lookup failure, VERR/VERW matrix, LAR/LSL values
# and failures, ARPL, task-switch LDTR load from TSS +0x60).
# Self-reports 24 "tN ok" lines then "pm-smoke done", exits status 11.
#
# Calibration: byte-identical to qemu-system-i386 -kernel on 22 of 24 tests
# (pm_qemu2.txt). Two divergences, same root: this qemu build (10.2.92,
# v11.0.0-rc2-12119-gaa7f0eb8d8-dirty) does NOT execute data-segment limit
# checks in TCG — t7 (ring-3 store past a GDT data segment limit) and t20
# check 1 (store past an LDT segment limit; LSL confirms limit 0x1ff, the
# store still lands) both let the store through where SDM vol.3 5.2.1/5.3
# mandate #GP. cemu faults and the judge below counts on cemu's output.
# QEMU (same build) DOES emulate A/D bits (t17) and computes LAR per the
# 00FxFF00 mask with the undefined nibble zeroed (t22).
# Pending: recalibrate t7/t20 against a clean QEMU build.
expected_pm=24
out=$(timeout 30 "$CEMU" --machine x86 --isa x86 "$dir/pm/pm_smoke.elf" 2>&1)
rc=$?
n_ok=$(echo "$out" | grep -c ' ok$')
n_bad=$(echo "$out" | grep -c 'BAD')
if [ "$rc" -eq 11 ] && [ "$n_ok" -eq "$expected_pm" ] && [ "$n_bad" -eq 0 ] &&
   echo "$out" | grep -q 'pm-smoke done'; then
  pass=$((pass + 1))
else
  fail=$((fail + 1))
  echo "FAIL (pm: rc=$rc, $n_ok ok, $n_bad bad)"
fi

# kvm-unit-tests 32-bit flat images (built from the v86 checkout by
# build_kut.sh; boot via multiboot, exit via 0xF4 debug-exit — payload 0
# (pass) shows up as status 1). taskswitch/taskswitch2 exercise the stage-3
# gate/task-switch semantics, cmpxchg8b the 0F C7 instruction, memory the
# vm.c allocator over 4MB pages, debug the SDM ch.17 DR breakpoints (32-bit
# port of the upstream 64-bit-only test — mode-independent semantics, see
# AGENTS.md D6). The kvm exit convention: report_summary
# failures call exit(failures) — payload 0 only when every test passed.
for t in taskswitch taskswitch2 cmpxchg8b memory debug; do
  timeout 60 "$CEMU" --machine x86 --isa x86 "$dir/$t.elf" > /dev/null 2>&1
  rc=$?
  if [ "$rc" -eq 1 ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "FAIL (kvm-$t: rc=$rc)"
  fi
done

# Realmode: suite self-reports PASS/FAIL lines. Stage 2 added the 8259 PIC
# + 8254 PIT so hlt wakes on IRQ0, and the whole suite runs: 126 PASS.
# QEMU reference (qemu-system-i386 -kernel realmode.elf) prints the same
# first 110 lines byte-identically and then stalls mid-suite (QEMU-side
# behavior, reproduced on every run); cemu runs the full suite. The goldens
# agree on every test QEMU actually reports.
#
# test_fninit asserts an x87 FPU is there (after fninit: fsw == 0 and
# fcw & 0x103f == 0x3f). This machine reports no FPU in CPUID (EDX.FPU = 0)
# and therefore executes the ESC opcodes as NOPs, exactly like a 386SX/486SX
# (D13); the test's memory words keep their preloaded values and it fails.
# That one failure is the no-FPU model, not a semantic gap -- QEMU passes it
# because QEMU's CPU does have an FPU.
#
# The process does NOT terminate on its own (the suite ends in a spin), so the
# run.sh judge works on output captured under a timeout.
expected_pass=126
out=$(timeout 60 "$CEMU" --machine x86 --isa x86 "$dir/realmode/realmode.elf" 2>&1)
# Note: cemu's [info] loader line has no trailing newline, gluing the first
# "PASS:" to it, so match PASS:/FAIL: anywhere in a line, not at line start.
n_pass=$(echo "$out" | grep -c 'PASS:')
n_fail=$(echo "$out" | grep -c 'FAIL:')
if [ "$n_pass" -eq "$expected_pass" ] && [ "$n_fail" -eq 1 ] &&
   echo "$out" | grep -q 'FAIL: fninit'; then
  pass=$((pass + 1))
else
  fail=$((fail + 1))
  echo "FAIL (realmode: $n_pass pass, $n_fail fail)"
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
