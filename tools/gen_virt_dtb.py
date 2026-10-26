#!/usr/bin/env python3
"""Regenerate src/board/virt_dtb.h from the blob already embedded in it,
adding the /soc framebuffer node the virt board hands Linux (the
"simple-framebuffer" DT binding: a bootloader-owned scanout buffer at a
fixed physical address).

The base tree is QEMU 10.2's own dump (see the header's provenance comment);
this tool is idempotent -- it injects the node only when absent -- and
records the parameters in the header so the tree stays reproducible.

Usage: gen_virt_dtb.py [width height stride]   (default 1024 768 4096,
base fixed at 0x8c000000, top of the 256 MiB DRAM minus the 4 MiB hole the
board's memory-node cap leaves for the scanout)
"""
import re
import struct
import sys

HDR = "src/board/virt_dtb.h"
FB_BASE = 0x8C000000
WIDTH, HEIGHT, STRIDE = (int(x) for x in sys.argv[1:4]) if len(sys.argv) > 3 else (1024, 768, 4096)

FDT_BEGIN, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def parse_header(text):
    m = re.search(r"kVirtDtb\[(\d+)\] = \{(.*?)\};", text, re.S)
    return int(m.group(1)), bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", m.group(2)))


def be32(v):
    return struct.pack(">I", v)


def cells(*vals):
    out = b""
    for v in vals:
        out += struct.pack(">Q", v) if v >> 32 else be32(v)
    return out


def inject(dtb):
    off_struct = struct.unpack_from(">I", dtb, 8)[0]
    off_strings = struct.unpack_from(">I", dtb, 12)[0]
    totalsize = struct.unpack_from(">I", dtb, 4)[0]
    size_strings = struct.unpack_from(">I", dtb, 32)[0]

    strings = dtb[off_strings:off_strings + size_strings]
    names = set(strings.split(b"\x00"))

    def intern(name):
        if name not in names:
            return None
        return strings.index(name + b"\x00")

    props = [
        (b"compatible", b"simple-framebuffer\x00"),
        (b"reg", struct.pack(">QQ", FB_BASE, STRIDE * HEIGHT)),
        (b"width", be32(WIDTH)),
        (b"height", be32(HEIGHT)),
        (b"stride", be32(STRIDE)),
        (b"format", b"a8r8g8b8\x00"),
    ]

    # walk to /soc's closing END_NODE; also prove the node is absent
    pos = off_struct
    depth = 0
    in_soc = 0
    insert_at = 0
    have_fb = 0
    while True:
        tok = struct.unpack_from(">I", dtb, pos)[0]
        pos += 4
        if tok == FDT_BEGIN:
            nm = dtb[pos:dtb.find(b"\x00", pos)]
            if depth == 1 and nm == b"soc":
                in_soc = 1
            if in_soc and nm.startswith(b"framebuffer"):
                have_fb = 1
            depth += 1
            pos = (pos + len(nm) + 1 + 3) & ~3
        elif tok == FDT_END_NODE:
            depth -= 1
            if in_soc and depth == 1:
                insert_at = pos - 4
                in_soc = 0
            if depth == 0:
                break
        elif tok == FDT_PROP:
            ln = struct.unpack_from(">I", dtb, pos)[0]
            noff = struct.unpack_from(">I", dtb, pos + 4)[0]
            nm = dtb[off_strings + noff:dtb.find(b"\x00", off_strings + noff)]
            if in_soc and depth == 2 and nm == b"format":
                have_fb = 1  # already injected on a previous run
            pos += 8   # len + nameoff (the token itself is consumed above)
            pos = (pos + ln + 3) & ~3
        elif tok != FDT_NOP:
            break
    assert insert_at, "tree has no /soc node"
    if have_fb:
        # Rerun: the node is already there. Still repair the header's
        # structure length from the layout -- blobs written before this
        # field was maintained carry a value short by the injected node,
        # and libfdt (OpenSBI's fdt_open_into) repacks from it.
        out = bytearray(dtb)
        struct.pack_into(">I", out, 36, off_strings - off_struct)
        return bytes(out)

    # serialize the node; new names append to the strings block
    new_names = b""
    body = b""

    def nameoff(name):
        nonlocal new_names
        off = intern(name)
        if off is not None:
            return off
        off = size_strings + len(new_names)
        new_names += name + b"\x00"
        names.add(name)
        return off

    name = b"framebuffer@%x" % FB_BASE
    raw = name + b"\x00"
    body += be32(FDT_BEGIN) + raw + b"\x00" * ((4 - len(raw) % 4) % 4)
    for nm, val in props:
        body += be32(FDT_PROP) + be32(len(val)) + be32(nameoff(nm)) + val
        body += b"\x00" * ((4 - len(val) % 4) % 4)
    body += be32(FDT_END_NODE)
    grow = len(body) + len(new_names)

    out = bytearray(dtb)
    out.extend(b"\x00" * grow)
    out[off_strings + len(body):totalsize + len(body)] = dtb[off_strings:totalsize]
    out[insert_at + len(body):off_strings + len(body)] = dtb[insert_at:off_strings]
    out[insert_at:insert_at + len(body)] = body
    out[off_strings + len(body) + size_strings:off_strings + len(body) + size_strings + len(new_names)] = new_names
    struct.pack_into(">I", out, 4, totalsize + grow)
    struct.pack_into(">I", out, 12, off_strings + len(body))
    struct.pack_into(">I", out, 32, size_strings + len(new_names))
    # The inserted node lives in the structure block: libfdt (OpenSBI's
    # fdt_open_into) repacks the blob from this field, so a stale value
    # truncates the struct tail.
    size_struct = struct.unpack_from(">I", dtb, 36)[0]
    struct.pack_into(">I", out, 36, size_struct + len(body))
    return bytes(out)


def emit(size, blob):
    rows = []
    for i in range(0, len(blob), 16):
        rows.append("    " + ", ".join("0x%02x" % b for b in blob[i:i + 16]) + ",")
    array = "\n".join(rows)
    return f"""#ifndef CEMU_BOARD_VIRT_DTB_H
#define CEMU_BOARD_VIRT_DTB_H

#include <stddef.h>
#include <stdint.h>

// Device tree blob captured from the real machine contract:
// qemu-system-riscv64 -machine virt,dumpdtb=virt.dtb (QEMU 10.2, 128 MiB),
// plus the /soc/framebuffer@8c000000 node this machine carries (the
// simple-framebuffer DT binding: {WIDTH}x{HEIGHT} a8r8g8b8, stride {STRIDE},
// at the top of DRAM above the memory node's cap). Regenerate with
// tools/gen_virt_dtb.py. The memory node's size cells are patched to the
// actual RAM size at machine creation.
static const size_t kVirtDtbSize = {size};
static const uint8_t kVirtDtb[{size}] = {{
{array}
}};

#endif  // CEMU_BOARD_VIRT_DTB_H
"""


text = open(HDR, encoding="utf-8").read()
_, blob = parse_header(text)
new = inject(blob)
open(HDR, "w", encoding="utf-8", newline="\n").write(emit(len(new), new))
print("virt_dtb.h: %d bytes, framebuffer %dx%d stride %d @ 0x%x"
      % (len(new), WIDTH, HEIGHT, STRIDE, FB_BASE))
