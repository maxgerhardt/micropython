"""Merge several ELFs into one flat flash image.

The CH32H417's OpenOCD flash driver mass-erases the whole chip on every
`program` command, so a dual-core build cannot flash its two images as two
separate operations -- the second wipes the first. They have to be combined
into a single image and written once.

Each ELF is converted to a raw binary and placed at its own load address,
relative to --base (default 0x00000000, the flash alias).

Lives in the port rather than in the outer repo's scripts/ because CI checks
out the micropython repository alone, and firmware.bin cannot be produced
without it.

Usage:
    python merge_flash.py -o combined.bin boot_v3f.elf firmware.elf
"""

import argparse
import os
import subprocess
import sys

# The toolchain prefix. CI supplies CROSS_COMPILE, and it is not the same
# toolchain there; a development machine falls back to the PlatformIO package
# store, which is where the WCH compiler lives locally.
CROSS = os.environ.get("CROSS_COMPILE")
if not CROSS:
    PIO_PKGS = os.environ.get(
        "PIO_PKGS", os.path.join(os.path.expanduser("~"), ".platformio", "packages")
    )
    CROSS = os.path.join(PIO_PKGS, "toolchain-riscv", "bin", "riscv-wch-elf-")


def lma_of(elf):
    """Lowest load address among allocated, loadable sections."""
    out = subprocess.run(
        [CROSS + "objdump", "-h", elf], capture_output=True, text=True, check=True
    ).stdout
    lmas = []
    lines = out.splitlines()
    for i, line in enumerate(lines):
        parts = line.split()
        # "Idx Name Size VMA LMA FileOff Algn" then flags on the next line
        if len(parts) >= 6 and parts[0].isdigit():
            flags = lines[i + 1] if i + 1 < len(lines) else ""
            if "LOAD" in flags and "ALLOC" in flags and int(parts[2], 16) > 0:
                lmas.append(int(parts[4], 16))
    if not lmas:
        raise SystemExit("no loadable sections in " + elf)
    return min(lmas)


def to_bin(elf, out_path):
    subprocess.run([CROSS + "objcopy", "-O", "binary", elf, out_path], check=True)
    with open(out_path, "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elves", nargs="+")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--base", default="0x00000000")
    ap.add_argument("--pad", default="0xFF", help="fill byte for gaps between images")
    args = ap.parse_args()

    base = int(args.base, 0)
    pad = int(args.pad, 0)

    pieces = []
    for elf in args.elves:
        lma = lma_of(elf)
        if lma < base:
            raise SystemExit("{} loads at {:#x}, below base {:#x}".format(elf, lma, base))
        data = to_bin(elf, args.output + "." + os.path.basename(elf) + ".tmp")
        pieces.append((lma - base, data, elf, lma))

    pieces.sort(key=lambda p: p[0])

    # Reject overlaps rather than silently producing a corrupt image.
    for (off_a, data_a, elf_a, lma_a), (off_b, _, elf_b, lma_b) in zip(pieces, pieces[1:]):
        if off_a + len(data_a) > off_b:
            raise SystemExit(
                "images overlap: {} at {:#x}+{:#x} runs into {} at {:#x}".format(
                    elf_a, lma_a, len(data_a), elf_b, lma_b
                )
            )

    total = pieces[-1][0] + len(pieces[-1][1])
    image = bytearray([pad]) * total
    for off, data, elf, lma in pieces:
        image[off : off + len(data)] = data
        print("  {:#010x}  {:>7} bytes  {}".format(lma, len(data), elf))

    with open(args.output, "wb") as f:
        f.write(image)
    print("  -> {} ({} bytes)".format(args.output, total))

    for _, _, elf, _ in pieces:
        tmp = args.output + "." + os.path.basename(elf) + ".tmp"
        if os.path.exists(tmp):
            os.remove(tmp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
