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

# A failure reported by exit status alone is not diagnosable: rc cannot tell
# "the image would not open" from "the guest ran and disagreed", and the one
# run that came back all-rc=1 (stage 4 片 12) left no evidence at all. Print
# the tail of the guest's own output with every FAIL; FAIL_TAIL caps it (the
# realmode suite prints 126 PASS lines).
report_fail() {
  echo "FAIL ($1)"
  echo "$out" | tail -n "${FAIL_TAIL:-12}" | sed 's/^/    | /'
}

# Smoke: boot sector prints "cemu-x86-smoke" on COM1 (the QEMU dual-run golden
# output) and exits via debug-exit with value 5 -> status 11.
out=$(timeout 30 "$CEMU" --machine x86 --isa x86 "$dir/smoke/x86_smoke.bin" 2>&1)
rc=$?
if [ "$rc" -eq 11 ] && echo "$out" | grep -q cemu-x86-smoke; then
  pass=$((pass + 1))
else
  fail=$((fail + 1))
  report_fail "smoke: rc=$rc"
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
  report_fail "cga: rc=$rc"
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
    report_fail "rep-zero: rc=$rc"
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
    report_fail "pit-irq: rc=$rc"
  fi
else
  echo "SKIP (pit-irq: no nasm and no prebuilt probe bin)"
fi

# Machine reset (test/x86/probe/reset.asm, AGENTS.md D18): a boot sector that
# pulls the three ways a PC guest reboots the machine — port 0xCF9's reset
# control register (SeaBIOS pci_reboot), port 0x92 bit 0 (INIT_NOW) and the
# keyboard controller's 0xFE command (SeaBIOS i8042_reboot) — one per life,
# counting lives in CMOS, which survives a reset where low RAM does not (the
# firmware runs again in between). Only a machine that really restarted reaches
# the fourth life, which prints "reset ok" and exits 11.
# Dual run against qemu-system-i386: same transcript, same status 11. Measured
# one trigger at a time with -DTRIGGER=1|2|3 (each variant prints "no reset" if
# its trigger did nothing): QEMU's pc machine implements all three, so the
# three-life sequence completes there too.
resetbin="$dir/probe/reset.bin"
if [ ! -f "$resetbin" ] && command -v nasm > /dev/null 2>&1; then
  nasm -f bin -o "$resetbin" "$dir/probe/reset.asm"
fi
if [ -f "$resetbin" ]; then
  out=$(timeout 30 "$CEMU" --machine x86 --isa x86 "$resetbin" 2>&1)
  rc=$?
  if [ "$rc" -eq 11 ] && echo "$out" | grep -q 'reset ok'; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    report_fail "reset: rc=$rc"
  fi
else
  echo "SKIP (reset: no nasm and no prebuilt probe bin)"
fi

# BT/BTS/BTR/BTC bit-string addressing (test/x86/probe/bt_bits.asm): with a
# memory bit base the register form walks the address on by one operand per
# OperandSize bits (SDM vol.2 BT: "Effective Address + (4 * (BitOffset DIV
# 32))"), while the immediate form's high bits are the assembler's business
# and "the processor will ignore the high order bits" -- so the probe asserts
# both halves and fails on either kind of wrong answer. The register half
# guards the 片 12 fix: the linux.iso kernel's init_IRQ() installed zero IRQ
# gates because its system_vectors bitmap test read the same dword for all 224
# vectors. Dual run against qemu-system-i386: identical transcript, identical
# status 11.
btbin="$dir/probe/bt_bits.bin"
if [ ! -f "$btbin" ] && command -v nasm > /dev/null 2>&1; then
  nasm -f bin -o "$btbin" "$dir/probe/bt_bits.asm"
fi
if [ -f "$btbin" ]; then
  out=$(timeout 30 "$CEMU" --machine x86 --isa x86 "$btbin" 2>&1)
  rc=$?
  if [ "$rc" -eq 11 ] && echo "$out" | grep -q 'bt-bits ok'; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    report_fail "bt-bits: rc=$rc"
  fi
else
  echo "SKIP (bt-bits: no nasm and no prebuilt probe bin)"
fi

# CMOS RTC interrupts (test/x86/probe/rtc_irq.asm, AGENTS.md D18): status A
# RS=6 (1024 Hz) plus status B's PIE|AIE and an all-don't-care alarm, then the
# guest HALTS and counts which of status C's flags its IRQ8 handler sees. It
# reports pf/af/uf, never a tick count (that depends on host timing). The
# wait is bounded by hlt iterations, which is not the same wall time on the two
# emulators (~16ms each on cemu, ~1ms on QEMU) — the loop leaves as soon as AF
# arrives, so the bound only matters when the interrupt never comes.
# Dual run against qemu-system-i386: identical transcript, identical status 11.
rtcbin="$dir/probe/rtc_irq.bin"
if [ ! -f "$rtcbin" ] && command -v nasm > /dev/null 2>&1; then
  nasm -f bin -o "$rtcbin" "$dir/probe/rtc_irq.asm"
fi
if [ -f "$rtcbin" ]; then
  out=$(timeout 60 "$CEMU" --machine x86 --isa x86 "$rtcbin" 2>&1)
  rc=$?
  if [ "$rc" -eq 11 ] && echo "$out" | grep -q 'rtc-irq ok'; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    report_fail "rtc-irq: rc=$rc"
  fi
else
  echo "SKIP (rtc-irq: no nasm and no prebuilt probe bin)"
fi

# PS/2 mouse on the 8042's auxiliary port (test/x86/probe/ps2mouse.asm): the
# guest drives the protocol half itself — 0xa8/0xa7 gate the interface, the
# reset/identify answers and the wheel handshake set the device id, 0xe9 reads
# the status, and the 0xeb poll's packet pins the byte layout — and then the
# host half runs, which no guest can drive: CEMU_DEBUG=mouse= feeds the board's
# pointer sink exactly the way the display window does (a button press, then a
# movement carrying one wheel detent). IRQ12 is observed through the 8259
# slave's request register, so no IDT, no remap and no sti are involved.
# A multiboot ELF in the pm suite's shape rather than a boot sector: the checks
# do not fit in the single sector QEMU's BIOS loads from a drive image.
# Dual run against qemu-system-i386 -kernel (monitor: `mouse_button 1` then
# `mouse_move 10 10 -1` — its Y/Z axes are screen-oriented, so the same host
# movement is spelled with the signs flipped) passes with the same status 11 and
# a byte-identical packet. Three divergences, all recorded in progress.md and
# all in the report line rather than asserted: the power-on command byte (41 vs
# 47), an unknown command's answer (nothing vs the resend QEMU's own source
# has), and 0xa7 (QEMU keeps queueing the device's answer while this model stops
# it at the wire — the spec's reading, and v86's).
mouseelf="$dir/probe/ps2mouse.elf"
if [ ! -f "$mouseelf" ] && command -v nasm > /dev/null 2>&1; then
  nasm -f elf32 -o "$dir/probe/ps2mouse.o" "$dir/probe/ps2mouse.asm" &&
    ld.lld -m elf_i386 -T "$dir/probe/ps2mouse.ld" -o "$mouseelf" "$dir/probe/ps2mouse.o"
fi
if [ -f "$mouseelf" ]; then
  out=$(CEMU_DEBUG="mouse=0:0:1@100000,mouse=10:-10:1:1@100000" timeout 60 "$CEMU" \
        --machine x86 --isa x86 "$mouseelf" 2>&1)
  rc=$?
  if [ "$rc" -eq 11 ] && echo "$out" | grep -q 'unk=fe auxoff=00 irr=10 pkt=09000000290af601' &&
     echo "$out" | grep -q 'ps2mouse ok'; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    report_fail "ps2mouse: rc=$rc"
  fi
else
  echo "SKIP (ps2mouse: no nasm and no prebuilt probe elf)"
fi

# PS/2 keyboard on the 8042's keyboard port (test/x86/probe/ps2kbd.asm): the
# command half runs on the guest's own initiative (0xf0 query/select, 0xf2 ID,
# 0xee echo, 0xed/0xf3 parameters, 0xf4/0xf5, 0xff reset, an unknown command,
# and the translated replies — a translating 8042 turns "I am in set 2" into
# 0x41 and the MF2 ID into 0xab 0x41), and the translation half needs the host,
# so CEMU_DEBUG=key= presses 'a' and the Up arrow, press and release each. Three
# phases: translation on + set 2 -> the host's set-1 bytes; translation off +
# set 2 -> the set-2 encoding with its 0xf0 break prefix and 0xe0 extended
# prefix; translation off + set 1 -> set 1 again ("Set 1 should not be
# translated", aeb scancodes-10 §10.1).
# Dual run against qemu-system-i386 -kernel (monitor: `sendkey a 10` then
# `sendkey up 10`, repeated) passes with the same status 11, and its command
# half agrees on all 29 assertions. The sequences are asserted here rather than
# in the probe, and three divergences are printed rather than asserted: the
# set-3 answer (QEMU accepts set 3 and switches to it, this model claims sets 1
# and 2 only and draws the resend), the command byte's value (61 vs 47 — QEMU
# leaves its own bits in it), and the phase boundaries (QEMU's monitor events do
# not land on the probe's phase edges, so its sequences carry the same bytes
# rotated — the encodings themselves agree).
kbdelf="$dir/probe/ps2kbd.elf"
if [ ! -f "$kbdelf" ] && command -v nasm > /dev/null 2>&1; then
  nasm -f elf32 -o "$dir/probe/ps2kbd.o" "$dir/probe/ps2kbd.asm" &&
    ld.lld -m elf_i386 -T "$dir/probe/ps2kbd.ld" -o "$kbdelf" "$dir/probe/ps2kbd.o"
fi
if [ -f "$kbdelf" ]; then
  out=$(CEMU_DEBUG="key=0x1e@100000,key=0x1e:0:1@100000,key=0x48:1@100000,key=0x48:1:1@100000" \
        timeout 60 "$CEMU" --machine x86 --isa x86 "$kbdelf" 2>&1)
  rc=$?
  if [ "$rc" -eq 11 ] && echo "$out" | grep -q 's3=fe cmd=47 seq=1e9ee048e0c8 seq=1cf01ce075e0f075 seq=1e9ee048e0c8' &&
     echo "$out" | grep -q 'ps2kbd ok'; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    report_fail "ps2kbd: rc=$rc"
  fi
else
  echo "SKIP (ps2kbd: no nasm and no prebuilt probe elf)"
fi

# Microsoft serial mouse on COM1 (test/x86/probe/sermouse.asm): the device a
# 1985-era guest drives — no registers, no bus presence, it just pushes 3-byte
# packets out of the serial port as if they had arrived on the SIN pin. The host
# is what produces them (CEMU_DEBUG=mouse=, and -mouse serial routes the host
# pointer there instead of to the 8042's auxiliary port), so the probe is a
# receiver: it sets COM1 up the way such a driver does (1200 baud, 7 data bits,
# no parity, one stop bit, FIFO on) and asserts the two packets — a left-button
# press with no movement, then a movement with the button still held:
#
#   60 00 00   the button's: bit 6 always set, bit 5 left, no movement
#   6c 0a 36   the movement's: dx = +10, dy = -10 (low six bits 0x36 = -10 with
#              11 in byte 1's high pair)
#
# No QEMU dual run: the monitor's mouse commands go to the first registered
# mouse handler (the default PC's PS/2 mouse), so the msmouse chardev never
# receives them — measured, and written up in the probe's header. The layout
# rests on QEMU's chardev/msmouse.c, not on a double run.
mouselff="$dir/probe/sermouse.elf"
if [ ! -f "$mouselff" ] && command -v nasm > /dev/null 2>&1; then
  nasm -f elf32 -o "$dir/probe/sermouse.o" "$dir/probe/sermouse.asm" &&
    ld.lld -m elf_i386 -T "$dir/probe/sermouse.ld" -o "$mouselff" "$dir/probe/sermouse.o"
fi
if [ -f "$mouselff" ]; then
  out=$(CEMU_DEBUG="mouse=0:0:1@100000,mouse=10:-10:1@100000" \
        timeout 30 "$CEMU" --machine x86 --isa x86 -mouse serial "$mouselff" 2>&1)
  rc=$?
  if [ "$rc" -eq 11 ] && echo "$out" | grep -q 'pkt=6000006c0a36' &&
     echo "$out" | grep -q 'sermouse ok'; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    report_fail "sermouse: rc=$rc"
  fi
else
  echo "SKIP (sermouse: no nasm and no prebuilt probe elf)"
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
# CR0.WP=1, A/D bits on successful translations in both the 4KB walk and a
# 4MB leaf, ring-3 user/supervisor #PF ec=7), the LDT (LLDT/SLDT round trip,
# TI=1 data loads with limit enforcement, cleared-LDTR lookup failure,
# VERR/VERW matrix, LAR/LSL values and failures, ARPL, task-switch LDTR load
# from TSS +0x60) and a 16-bit TSS (JMP switch through a type-1 descriptor:
# IP/FLAGS/GPRs/selectors read and written at the fig 7-2 offsets).
# Self-reports 26 "tN ok" lines then "pm-smoke done", exits status 11.
#
# Calibration: byte-identical to qemu-system-i386 -kernel on 24 of 26 tests
# (pm_qemu4.txt). Two divergences, same root: this qemu build (10.2.92,
# v11.0.0-rc2-12119-gaa7f0eb8d8-dirty) does NOT execute data-segment limit
# checks in TCG — t7 (ring-3 store past a GDT data segment limit) and t20
# check 1 (store past an LDT segment limit; LSL confirms limit 0x1ff, the
# store still lands) both let the store through where SDM vol.3 5.2.1/5.3
# mandate #GP. cemu faults and the judge below counts on cemu's output.
# QEMU (same build) DOES emulate A/D bits (t17, t25) and computes LAR per the
# 00FxFF00 mask with the undefined nibble zeroed (t22).
# Pending: recalibrate t7/t20 against a clean QEMU build.
expected_pm=26
out=$(timeout 30 "$CEMU" --machine x86 --isa x86 "$dir/pm/pm_smoke.elf" 2>&1)
rc=$?
n_ok=$(echo "$out" | grep -c ' ok$')
n_bad=$(echo "$out" | grep -c 'BAD')
if [ "$rc" -eq 11 ] && [ "$n_ok" -eq "$expected_pm" ] && [ "$n_bad" -eq 0 ] &&
   echo "$out" | grep -q 'pm-smoke done'; then
  pass=$((pass + 1))
else
  fail=$((fail + 1))
  report_fail "pm: rc=$rc, $n_ok ok, $n_bad bad"
fi

# kvm-unit-tests 32-bit flat images (built from the v86 checkout by
# build_kut.sh; boot via multiboot, exit via 0xF4 debug-exit — payload 0
# (pass) shows up as status 1). taskswitch/taskswitch2 exercise the stage-3
# gate/task-switch semantics, cmpxchg8b the 0F C7 instruction, memory the
# vm.c allocator over 4MB pages, debug the SDM ch.17 DR breakpoints (32-bit
# port of the upstream 64-bit-only test — mode-independent semantics, see
# AGENTS.md D6), access the page-rights table (32-bit port of the upstream
# 64-bit-only x86/access.c; it is the test that covers "a ring-3 write to a
# user-readable read-only page faults whatever CR0.WP says" — the cell the
# 片 15 fix restored — plus, through its pde.bit13 axis, the reserved bits of
# a largepage PDE). The kvm exit convention:
# report_summary failures call exit(failures) — payload 0 only when every
# test passed.
#
# rmap_chain (also in upstream's i386 Makefile list) is not built: it maps its
# pages from 0xfffffa000 — a 64-bit address that truncates to 0xffffa000 here —
# and loops over fw_cfg's RAM_SIZE, so on a 32-bit machine the pointer wraps
# past 4GB and the loop overwrites the page tables it is building.
for t in taskswitch taskswitch2 cmpxchg8b memory debug access ioapic; do
  out=$(timeout 60 "$CEMU" --machine x86 --isa x86 "$dir/$t.elf" 2>&1)
  rc=$?
  # Exit status alone is not enough. A guest that exits cleanly reports payload
  # 0 -> status 1, and the run is over; but cemu's own fatal stop used to exit
  # 1 as well, so a guest that died on a missing feature (taskswitch2 reaching
  # VM86) was scored green, and a FAIL line printed before it never mattered.
  # Require the clean exit *and* the guest's own output to be free of failures:
  # XFAIL is the suite's "expected failure" (memory's clflush/sfence/...), so
  # only a FAIL not preceded by X counts.
  if [ "$rc" -eq 1 ] && ! echo "$out" | grep -qE '(^|[^X])FAIL|\[fatal\]'; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    report_fail "kvm-$t: rc=$rc"
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
  report_fail "realmode: $n_pass pass, $n_fail fail"
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
