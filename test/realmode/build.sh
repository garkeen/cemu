#!/bin/sh
# Builds kvm-unit-tests x86/realmode (GPLv2, from v86/tests/kvm-unit-tests)
# into an ELF32 image that both cemu and qemu-system-i386 -kernel can load.
#
# mingw adaptations:
# - as (pe-i386) lacks .pushsection/.previous, so the compiler-generated
#   assembly is flattened: .pushsection X -> .section X and .popsection ->
#   .section .text. All uses in realmode.s are non-nested pairs from MK_INSN.
# - ld only has the i386pe emulation; test/realmode/realmode.lds places the
#   .text.insn/.data.insn/.rdata sections explicitly.
# - objcopy converts the PE image to ELF32; e_entry is patched from the
#   start symbol because the conversion does not carry the PE entry point.
set -e
cd "$(dirname "$0")"
K=../../../v86/tests/kvm-unit-tests/x86

# - C symbols on pe-i386 carry a leading underscore, which would break the
#   file's inline assembly references; -fno-leading-underscore restores the
#   ELF-style naming the test was written for.
gcc -m32 -std=gnu99 -ffreestanding -fno-asynchronous-unwind-tables \
    -fno-stack-protector -fno-leading-underscore -S -o realmode.gen.s \
    "$K/realmode.c"
sed 's/\.pushsection \.data\.insn/.section .data.insn/;
     s/\.pushsection \.text\.insn/.section .text.insn/;
     s/\.popsection/.section .text/' realmode.gen.s > realmode.flat.s
gcc -m32 -c -fno-leading-underscore -o realmode.o realmode.flat.s
gcc -m32 -nostdlib -Wl,-T,realmode.lds -o realmode.exe realmode.o

start_addr=$(nm realmode.exe | awk '$3 == "start" { print $1 }')
objcopy -O elf32-i386 --remove-section .drectve realmode.exe realmode.elf
python - "$start_addr" << 'EOF'
import struct, sys
entry = int(sys.argv[1], 16)
with open("realmode.elf", "r+b") as f:
    f.seek(0x18)  # e_entry, ELF32
    f.write(struct.pack("<I", entry))
print("realmode.elf: e_entry = 0x%x" % entry)
EOF
