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

# Transcript probe cell. These probes do not self-judge: they print what the
# machine answered, and the judgement is the comparison. The calibration file
# (probe/<stem>_qemu.txt) carries the same sector's transcript from each model as
# two machine-readable lines, `cemu ...` and `qemu ...`, with the prose around
# them explaining every field.
#
#   mode=ref   the two lines are equal and cemu's live output must equal them:
#              the reference model pins the answer.
#   mode=lock  the two lines differ at fields this card takes from the spec
#              instead of from the reference (AGENTS.md §X's D17); cemu's live
#              output must equal *its* line, so any change in the card's answers
#              is caught, and the divergence stays on the record instead of in a
#              silent exception list.
#
# Leading/trailing blanks are trimmed on both sides: the guest's own lines carry
# the spacing it prints, and a golden must not fail because of one.
probe_cell() {
  local stem=$1 status=$2 mode=$3
  local bin="$dir/probe/$stem.bin" cal="$dir/probe/${stem}_qemu.txt"
  if [ ! -f "$bin" ] && command -v nasm > /dev/null 2>&1; then
    nasm -f bin -o "$bin" "$dir/probe/$stem.asm"
  fi
  if [ ! -f "$bin" ] || [ ! -f "$cal" ]; then
    echo "SKIP ($stem: needs nasm and the calibration file $cal)"
    return
  fi
  out=$(timeout 30 "$CEMU" --machine x86 --isa x86 "$bin" 2>&1)
  local rc=$?
  local want_c want_q got tok
  want_c=$(awk '/^cemu /{sub(/^cemu /,""); print; exit}' "$cal" | sed 's/[[:space:]]*$//')
  want_q=$(awk '/^qemu /{sub(/^qemu /,""); print; exit}' "$cal" | sed 's/[[:space:]]*$//')
  tok=${want_c%% *}
  got=$(printf '%s\n' "$out" | grep -m1 -- "$tok" | sed "s/.*$tok/$tok/" | sed 's/[[:space:]]*$//')
  local want="$want_c"
  [ "$mode" = ref ] && want="$want_q"
  if [ "$rc" -eq "$status" ] && [ -n "$got" ] && [ "$got" = "$want" ]; then
    if [ "$mode" = ref ] && [ "$want_c" != "$want_q" ]; then
      fail=$((fail + 1))
      report_fail "$stem: calibration says the models must agree, but they do not"
      return
    fi
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    report_fail "$stem: rc=$rc want rc=$status; transcript got='$got' want='$want'"
  fi
}

# Multiboot ELF cell: same transcript contract as probe_cell, with the
# nasm -f elf32 + ld.lld build the pm-suite probes use. The calibration file
# carries the cemu and QEMU lines; mode=lock records a divergence settled by
# the manual (the prose in the file says why).
elf_cell() {
  local stem=$1 status=$2 mode=$3
  local elf="$dir/probe/$stem.elf" cal="$dir/probe/${stem}_qemu.txt"
  if [ ! -f "$elf" ] && command -v nasm > /dev/null 2>&1 &&
     command -v ld.lld > /dev/null 2>&1; then
    local ld="$dir/probe/$stem.ld"
    [ -f "$ld" ] || ld="$dir/probe/vbe_mode.ld"
    nasm -f elf32 -o "$dir/probe/$stem.o" "$dir/probe/$stem.asm" &&
      ld.lld -m elf_i386 -T "$ld" -o "$elf" "$dir/probe/$stem.o"
  fi
  if [ ! -f "$elf" ] || [ ! -f "$cal" ]; then
    echo "SKIP ($stem: needs nasm/ld.lld and the calibration file $cal)"
    return
  fi
  out=$(timeout 30 "$CEMU" --machine x86 --isa x86 "$elf" 2>&1)
  local rc=$?
  local want_c want_q got tok
  want_c=$(awk '/^cemu /{sub(/^cemu /,""); print; exit}' "$cal" | sed 's/[[:space:]]*$//')
  want_q=$(awk '/^qemu /{sub(/^qemu /,""); print; exit}' "$cal" | sed 's/[[:space:]]*$//')
  tok=${want_c%% *}
  got=$(printf '%s\n' "$out" | grep -m1 -- "$tok" | sed "s/.*$tok/$tok/" | sed 's/[[:space:]]*$//')
  local want="$want_c"
  [ "$mode" = ref ] && want="$want_q"
  if [ "$rc" -eq "$status" ] && [ -n "$got" ] && [ "$got" = "$want" ]; then
    if [ "$mode" = ref ] && [ "$want_c" != "$want_q" ]; then
      fail=$((fail + 1))
      report_fail "$stem: calibration says the models must agree, but they do not"
      return
    fi
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    report_fail "$stem: rc=$rc want rc=$status; transcript got='$got' want='$want'"
  fi
}

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

# The sense of the two repeat prefixes, measured from raw bytes
# (test/x86/probe/rep_sense.asm). F2/REPNZ repeats while ZF=0 and F3/REPZ while
# ZF=1, and CMPS/SCAS are the only instructions that read them; a model that maps
# the bytes onto the wrong senses ends every name scan one byte early or late,
# which is a control-flow divergence in a DOS guest, not a slow one. The probe
# carries `db 0xf2,0xa6` rather than a mnemonic because assembler conventions for
# the spelling are exactly what was under investigation -- and were wrong here
# once (see probe/rep_sense_qemu.txt). MOVS is the control: no flag test may
# reach it. ref cell: both models answer the same transcript.
probe_cell rep_sense 13 ref

# LOOP/LOOPE/LOOPNE/JCXZ and the INC/DEC flag contract
# (test/x86/probe/loop_sense.asm): SDM vol.2 fixes that INC/DEC leave CF and set
# SF/ZF/AF/PF/OF, that the counter is decremented before the test, and that JCXZ
# tests at the address size. Case d is the one a wrong model cannot hide: an
# `inc si` sits between the compare and the LOOPNE, so the ZF the loop reads is
# INC's, and the scan runs to its full count. These five instructions are the
# whole inner loop of the Windows 1.01 kernel's WIN.INI canonicalizer, which is
# why the case list is shaped like that parser. ref cell: identical transcripts.
probe_cell loop_sense 15 ref

# The full 16-bit ModRM matrix (test/x86/probe/addr16_matrix.asm): the eight
# r/m forms under mod=0/1/2, with DS and SS given different bases so the
# default segment is part of every case. r/m 0,1,4,5,6(mod=0),7 must use DS;
# r/m 2,3,6(mod=1/2) must use SS. The probe is self-contained (no firmware),
# stores a unique marker through each EA and reads the expected segment at the
# expected offset. ref cell: both models print one line and exit 5.
probe_cell addr16_matrix 11 ref

# The prefix and arithmetic edges (test/x86/probe/addr16_seg.asm): ES/DS/SS/CS
# overrides on the same effective offset, sign-extended negative disp8 with DS
# and SS defaults, the 64K wrap of BP+SI, BX+disp16 and BP+disp8, and the exact
# Windows 1.01 encoding [bp+si-0x9c] with a negative disp16. The matrix above
# pins the 24 table rows; this cell pins the prefix, the displacement sign and
# the carry out of the 16-bit sum. ref cell: both models print one line and
# exit 5.
probe_cell addr16_seg 11 ref

# Mode 6's aperture (test/x86/probe/vga_cga.asm): SeaBIOS's stdvgamodes shape
# -- one plane, odd/even addressing, the window at 0xB8000, graphics mode --
# programmed by the probe itself (no firmware). The probe fills 4KB a byte at a
# time, then reads it back at the write addresses, at the other parity, and
# after a word-at-a-time fill; a model that aliases parity differently or
# splits word writes differently prints a different transcript. ref cell: both
# models print the same line and exit 5 (status 11).
probe_cell vga_cga 11 ref

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

# VGA family core, guest-driven (test/x86/probe/vga_core.asm). No firmware: the
# probe is a multiboot image that programs the card's own register file, so it
# replaces the BIOS-driven vga_probe.asm that went away with the SeaBIOS cell.
# What it pins: the four index latches read back what was written, a zero
# written into each register reads back as zero (the one value every model's
# write mask keeps), the attribute index flip-flop is reset by a 0x3DA read, and
# a DAC entry survives a write/read-index round trip. R11 (index 0x11) bit 7 is
# cleared first because it protects R0-R7.
#
# Dual run: qemu-system-i386 -kernel vga_core.elf -vga std -display none
# -no-reboot -serial file:... prints the same "vga-core ok"
# (test/x86/probe/vga_core_qemu.txt).
vgacoreelf="$dir/probe/vga_core.elf"
if [ ! -f "$vgacoreelf" ] && command -v nasm > /dev/null 2>&1; then
  nasm -f elf32 -o "$dir/probe/vga_core.o" "$dir/probe/vga_core.asm" &&
    ld.lld -m elf_i386 -T "$dir/probe/vga_core.ld" -o "$vgacoreelf" \
      "$dir/probe/vga_core.o"
fi
if [ -f "$vgacoreelf" ]; then
  out=$(timeout 30 "$CEMU" --machine x86 --isa x86 "$vgacoreelf" 2>&1)
  rc=$?
  if [ "$rc" -eq 11 ] && echo "$out" | grep -q 'vga-core ok'; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    report_fail "vga-core: rc=$rc"
  fi
else
  echo "SKIP (vga-core: no nasm and no prebuilt probe elf)"
fi

# DISPI, VBE's index/data pair at 0x1CE/0x1CF (test/x86/probe/vga_dispi.asm).
# What it pins is the register file's bound: `dispi[11]` sits directly in front
# of the card's PciDevice, so an index naming no register turned a write to the
# data port into a write into PCI config space -- two outw's from any guest. The
# probe attacks with 0x0f and 0x8f, then reads 00:02.0's vendor/device back
# through 0xCF8/0xCFC, and closes by showing a register that does exist still
# stores (so the bound cannot be satisfied by refusing everything). It also
# carries the ROM's own handshake (bochsvga.c:309-313) and the read-only
# VIDEO_MEMORY_64K.
#
# Dual run: the same binary on qemu-system-i386 -vga std agrees on every step,
# once the card is given the memory the reference card reports (16MB -- BAR0 and
# DISPI VIDEO_MEMORY_64K both come from kVgaVramSize). The capture in
# test/x86/probe/vga_dispi_qemu.txt was made while this card still reported
# 256KB, and also records that step 2's answer is what sent the reference-claim
# this file used to carry back to the reference.
dispibin="$dir/probe/vga_dispi.bin"
if [ ! -f "$dispibin" ] && command -v nasm > /dev/null 2>&1; then
  nasm -f bin -o "$dispibin" "$dir/probe/vga_dispi.asm"
fi
if [ -f "$dispibin" ]; then
  out=$(timeout 30 "$CEMU" --machine x86 --isa x86 "$dispibin" 2>&1)
  rc=$?
  if [ "$rc" -eq 11 ] && echo "$out" | grep -q 'vga-dispi ok'; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    report_fail "vga-dispi: rc=$rc"
  fi
else
  echo "SKIP (vga-dispi: no nasm and no prebuilt probe bin)"
fi

# The VGA function's PCI BAR identity (test/x86/probe/vga_bar.asm): Region 2
# must size as a 4KB *memory* BAR (an I/O BAR would end in ...001) and Region 0
# as a 16MB 32-bit prefetchable memory BAR (...008; QEMU registers it with
# PCI_BASE_ADDRESS_MEM_PREFETCH only). Both models answer `vga-bar ok`; the
# probe self-judges through the sizing protocol. The BAR window itself is not
# exercised here -- a real-mode guest cannot reach the high address firmware
# assigns -- so VgaBarWritten's remove-on-move path stays a code-review item.
vgabarbin="$dir/probe/vga_bar.bin"
if [ ! -f "$vgabarbin" ] && command -v nasm > /dev/null 2>&1; then
  nasm -f bin -o "$vgabarbin" "$dir/probe/vga_bar.asm"
fi
if [ -f "$vgabarbin" ]; then
  out=$(timeout 30 "$CEMU" --machine x86 --isa x86 "$vgabarbin" 2>&1)
  rc=$?
  if [ "$rc" -eq 11 ] && echo "$out" | grep -q 'vga-bar ok'; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    report_fail "vga-bar: rc=$rc"
  fi
else
  echo "SKIP (vga-bar: no nasm and no prebuilt probe bin)"
fi

# The register file's read-back shapes (test/x86/probe/vga_readback.asm): the
# attribute index latch and the flip-flop, the CRTC index latch width, the
# sequencer and graphics write masks, the feature-control latch (read at 0x3CA --
# reading 0x3DA answers input status 1, which is wherever the raster is at that
# instant and therefore measures the clock rather than the latch), and whether the
# retrace bit is observable at all to a polling guest (case g reads 0x3DA until
# both phases have been seen, bounded at ~1.3M reads, printed as Y/N -- the
# vertical pulse is only CR10..CR11's few lines, so a fixed 200-sample count can
# miss it). ref cell: the whole transcript, including the 0x3BA decode under misc
# bit 0, agrees with QEMU.
probe_cell vga_readback 19 ref

# The whole register file as the card answers it at boot
# (test/x86/probe/vga_census.asm): 60 reads over CRTC/graphics/sequencer/attribute
# plus five deterministic cases, so a divergence names the register rather than
# the symptom -- this is the byte block SeaVGABIOS turns into the "EGA/VGA
# information" that int 10 AX=1000/1001 returns, and a display driver decides its
# mode from those bytes. lock cell: every field matches QEMU except CR14/CR15,
# which are the cursor position the firmware left behind. The file also records
# why the earlier "13 attribute-controller differences" reading was retracted:
# `in al,0x3da` does not encode, so the flip-flop was never reset on either model.
probe_cell vga_census 23 lock

# The graphics picture the register file describes (test/x86/probe/vga_paint.asm).
# No firmware: the multiboot probe programs SeaBIOS's own mode 0x10 tables
# (sequ_0e/crtc_0f/actl_10/grdc_0d plus the run-time line offset), paints through
# the aperture with write mode 2 -- one byte per pixel byte, the graphics
# controller expanding the colour across the planes, which is the shape the
# card's latched write path serves (QEMU's vga_draw_line4 reads one dword per
# pixel byte) -- and reports what the card reads back: the register file it
# holds and the plane bytes of five painted pixel bytes. The picture itself is
# judged host-side and headless against the ruler the probe paints: cemu's
# CEMU_DEBUG=fbdump=vga_paint.ppm (640x350; entry 15 white at both edges,
# entry 15 in pixels 8-15 on odd rows, entry 15 in pixels 16-23 where
# (row >> 1) & 1, row 100 all entry 15, row 300 all entry 3) against QEMU's
# `screendump` of the same image.
#
# Dual run: qemu-system-i386 -machine pc -m 32 -kernel vga_paint.elf -vga std
# -display none -no-reboot -serial file:vga_paint_qemu.txt prints the same
# transcript, byte for byte.
vgapaintelf="$dir/probe/vga_paint.elf"
if [ ! -f "$vgapaintelf" ] && command -v nasm > /dev/null 2>&1; then
  nasm -f elf32 -o "$dir/probe/vga_paint.o" "$dir/probe/vga_paint.asm" &&
    ld.lld -m elf_i386 -T "$dir/probe/vga_paint.ld" -o "$vgapaintelf" \
      "$dir/probe/vga_paint.o"
fi
if [ -f "$vgapaintelf" ]; then
  out=$(timeout 30 "$CEMU" --machine x86 --isa x86 "$vgapaintelf" 2>&1)
  rc=$?
  if [ "$rc" -eq 11 ] &&
     echo "$out" | grep -q 'cr00,01,09,13=5f,4f,40,28,12=5d,1f ar10,12=01,0f sr2,4=0f,06 gr5=02 p0=ffffffff r1p0=ff,ff,ff r100p0=ff,ff,ff' &&
     echo "$out" | grep -q 'vga-paint ok'; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    report_fail "vga-paint: rc=$rc"
  fi
else
  echo "SKIP (vga-paint: no nasm and no prebuilt probe elf)"
fi

# VBE mode programming through DISPI (test/x86/probe/vbe_mode.asm): the
# sanitized geometry registers, chain-4/bank aperture, and the packed
# 16/24/32bpp pixel bytes. Dual run is byte-identical on the shared checks;
# GETCAPS maxima diverge (QEMU 11 16000x12000 vs the ROM's 2560x1600) and the
# calibration file records both.
elf_cell vbe_mode 11 ref

# Split screen and horizontal pel panning (test/x86/probe/vga_split.asm):
# vga_paint's ruler with R18=175 and AR13=2. cemu's fbdump shows the split at
# displayed row 276 and the two-pixel shift; QEMU accepts the same registers.
elf_cell vga_split 11 ref

# ATA LBA-48 and READ MULTIPLE (test/x86/probe/ata_multi.asm): the probe is its
# own 1KiB disk (sector 1 is the fixture), attached as the primary master from
# a temporary copy so the boot image stays read-only. IDENTIFY fields,
# SET MULTIPLE, one two-sector DRQ block, and the 48-bit high-then-low task
# file sequence. Dual run is byte-identical.
atabin="$dir/probe/ata_multi.bin"
if [ ! -f "$atabin" ] && command -v nasm > /dev/null 2>&1; then
  nasm -f bin -o "$atabin" "$dir/probe/ata_multi.asm"
fi
if [ -f "$atabin" ] && command -v mktemp > /dev/null 2>&1; then
  ata_tmp=$(mktemp -d "${TMPDIR:-/tmp}/cemu-ata.XXXXXX" 2>/dev/null || true)
  ataimg="$ata_tmp/ata_multi.img"
  if [ -n "$ata_tmp" ] && cp "$atabin" "$ataimg"; then
    out=$(timeout 30 "$CEMU" --machine x86 --isa x86 -hda "$ataimg" "$atabin" 2>&1)
    rc=$?
    if [ "$rc" -eq 11 ] && echo "$out" | grep -q 'ata-multi ok'; then
      pass=$((pass + 1))
    else
      fail=$((fail + 1))
      report_fail "ata-multi: rc=$rc"
    fi
  else
    echo "SKIP (ata-multi: missing mktemp/cp)"
  fi
  [ -n "$ata_tmp" ] && rm -rf "$ata_tmp"
else
  echo "SKIP (ata-multi: no nasm and no prebuilt probe bin)"
fi

# PIIX4 PM1a fixed events, SCI and S5 (test/x86/probe/pm_sci.asm). Lock cell:
# ACPI 4.8's GBL_RLS behavior is implemented here and QEMU stores the bit
# instead; the calibration file records the divergence.
elf_cell pm_sci 0 lock

# I/O APIC NMI delivery (test/x86/probe/nmi_ioapic.asm), PIT IRQ0 -> I/O APIC
# pin 0 -> NMI. Lock cell: QEMU 11 accepts the entry but does not raise the
# CPU NMI flag in this configuration; cemu follows 82093AA §3.2.4.
elf_cell nmi_ioapic 11 lock

# Floppy controller + 8237 channel 2, guest-driven (test/x86/probe/fdc_dma.asm).
# No firmware and no BIOS: the multiboot probe issues the 8272A commands and
# programs the DMA channel itself (82077AA datasheet command and result
# phases; 8237A datasheet mode, page latch and mask), on a blank 1.44 MiB raw
# image the suite generates itself. One sector is read (the medium's zeros),
# written with 0x55 and read back, so both DMA directions, the seven-byte
# result phase and the channel's terminal-count bit are all pinned.
#
# Dual run: qemu-system-i386 -machine pc -m 32 -kernel fdc_dma.elf
# -drive file=<image>,format=raw,if=floppy -display none -no-reboot
# -serial file:... prints the same transcript (test/x86/probe/fdc_dma_qemu.txt;
# QEMU has no debug-exit, so the serial line is the verdict).
fdcdmaelf="$dir/probe/fdc_dma.elf"
if [ ! -f "$fdcdmaelf" ] && command -v nasm > /dev/null 2>&1; then
  nasm -f elf32 -o "$dir/probe/fdc_dma.o" "$dir/probe/fdc_dma.asm" &&
    ld.lld -m elf_i386 -T "$dir/probe/fdc_dma.ld" -o "$fdcdmaelf" \
      "$dir/probe/fdc_dma.o"
fi
if [ -f "$fdcdmaelf" ]; then
  if command -v mktemp >/dev/null 2>&1 && command -v dd >/dev/null 2>&1; then
    fdc_tmp=$(mktemp -d "${TMPDIR:-/tmp}/cemu-fdc.XXXXXX" 2>/dev/null || true)
  else
    fdc_tmp=
  fi
  fdcimg="$fdc_tmp/fdc_blank.img"
  if [ -n "$fdc_tmp" ] && dd if=/dev/zero of="$fdcimg" bs=512 count=2880 2>/dev/null; then
    out=$(timeout 30 "$CEMU" --machine x86 --isa x86 -fda "$fdcimg" "$fdcdmaelf" 2>&1)
    rc=$?
    if [ "$rc" -eq 11 ] && echo "$out" | grep -q 'fdc-dma ok'; then
      pass=$((pass + 1))
    else
      fail=$((fail + 1))
      report_fail "fdc-dma: rc=$rc"
    fi
  else
    echo "SKIP (fdc-dma: missing mktemp/dd)"
  fi
  [ -n "$fdc_tmp" ] && rm -rf "$fdc_tmp"
else
  echo "SKIP (fdc-dma: no nasm and no prebuilt probe elf)"
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
