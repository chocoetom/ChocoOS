#!/usr/bin/env python3
# Count exact-match pixels of one RGB color inside a binary PPM (P6) file.
# Used by scripts/test-fileman.sh; stdlib only (no PIL dependency).
# Usage: python ppm_count.py <file.ppm> <RRGGBB>
import sys


def main():
    path, hexcol = sys.argv[1], sys.argv[2]
    with open(path, "rb") as f:
        if f.readline().strip() != b"P6":
            sys.exit("not a P6 PPM")
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        _w, _h = map(int, line.split())
        f.readline()  # maxval
        data = f.read()
    want = bytes((int(hexcol[0:2], 16), int(hexcol[2:4], 16), int(hexcol[4:6], 16)))
    n = 0
    for i in range(0, len(data) - 2, 3):
        if data[i] == want[0] and data[i + 1] == want[1] and data[i + 2] == want[2]:
            n += 1
    print(n)


main()
