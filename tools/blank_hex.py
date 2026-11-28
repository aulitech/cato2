#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Writes an Intel HEX file of erased flash (0xFF) over the given ranges.

    blank_hex.py out.hex 0x76000:0x79000 0xe8000:0xea000

Merged into usb-upgrade.uf2 by the build, so that writing it to a board also
clears what an earlier flash layout left where MCUboot keeps its notes.
"""

import sys

from intelhex import IntelHex


def main():
    out, ranges = sys.argv[1], sys.argv[2:]
    if not ranges:
        sys.exit(__doc__)

    ih = IntelHex()
    for item in ranges:
        start, end = (int(value, 0) for value in item.split(":"))
        ih.puts(start, b"\xff" * (end - start))
    ih.write_hex_file(out)


if __name__ == "__main__":
    main()
