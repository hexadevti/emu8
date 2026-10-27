#!/usr/bin/env python3
"""make_icon.py - write icon.bmp, emu8's 32x32 icon for PicoMultiLoader's Home screen.

    python tools/pml-app/make_icon.py            # -> tools/pml-app/icon.bmp

The icon is a 4-bit BMP whose pixel values are PicoMultiLoader palette *roles*, painted in the
user's theme colours (PicoMultiLoader docs/apps.md, "Icons"). Edit the grid below, one character
per pixel, and run this again. The BMP's own palette is the default (Graphite) theme, so the file
also looks right in an image viewer.
"""
import os
import struct

ROLES = {   # character -> role index
    '.': 0,   # clear: the tile shows through
    'P': 1,   # PANEL
    'F': 2,   # FIELD
    'L': 3,   # LINE
    'D': 4,   # DIM
    'f': 5,   # FG
    'B': 6,   # BRIGHT
    'A': 7,   # ACCENT
    'a': 8,   # ACCDK
    'G': 9,   # OK (green)
    'W': 10,  # WARN (amber)
    'R': 11,  # ERR (red)
    'C': 12,  # ACC2
}
GRAPHITE = [0x000000, 0x172030, 0x212c40, 0x33415c, 0x7a88a3, 0xd5ddeb, 0xffffff, 0x4cc3ff,
            0x1d5f86, 0x5fd98a, 0xffb347, 0xff6b6b, 0xb48cff, 0x4cc3ff, 0xb48cff, 0x5fd98a]

ICON = """
................................
................................
...LLLLLLLLLLLLLLLLLLLLLLLLLL...
...LFFFFFFFFFFFFFFFFFFFFFFFFL...
...LFLLLLLLLLLLLLLLLLLLLLLLFL...
...LFLPPPPPPPPPPPPPPPPPPPPLFL...
...LFLPPPPPPPAAAAAAPPPPPPPLFL...
...LFLPPPPPPAAPPPPAAPPPPPPLFL...
...LFLPPPPPPAAPPPPAAPPPPPPLFL...
...LFLPPPPPPAAPPPPAAPPPPPPLFL...
...LFLPPPPPPPAAAAAAPPPPPPPLFL...
...LFLPPPPPPAAPPPPAAPPPPPPLFL...
...LFLPPPPPPAAPPPPAAPPPPPPLFL...
...LFLPPPPPPAAPPPPAAPPPPPPLFL...
...LFLPPPPPPAAPPPPAAPPPPPPLFL...
...LFLPPPPPPPAAAAAAPPPPPPPLFL...
...LFLPPPPPPPPPPPPPPPfffPPLFL...
...LFLPPPPPPPPPPPPPPPPPPPPLFL...
...LFLLLLLLLLLLLLLLLLLLLLLLFL...
...LFFFFFFFFFFFFFFFFFFFFFFFFL...
...LFFGFFFFFFFFFFFFFRWGAFFFFL...
...LLLLLLLLLLLLLLLLLLLLLLLLLL...
.............DDDDDD.............
................................
.LLLLLLLLLLLLLLLLLLLLLLLLLLLLLL.
.LFFFFFFFFFFFFFFFFFFFFFFFFFFFFL.
.LFfFfFfFfFfFfFfFfFfFfFfFfFfFFL.
.LFFFFFFFFFFFFFFFFFFFFFFFFFFFFL.
.LFFDDDDDDDDDDDDDDDDDDDDDDDDFFL.
.LLLLLLLLLLLLLLLLLLLLLLLLLLLLLL.
................................
................................
"""


def main():
    rows = ICON.strip().splitlines()
    assert len(rows) == 32 and all(len(r) == 32 for r in rows), 'the grid must be 32 x 32'
    pixels = b''
    for row in reversed(rows):   # BMP rows go bottom-up
        idx = [ROLES[c] for c in row]
        pixels += bytes(idx[i] << 4 | idx[i + 1] for i in range(0, 32, 2))
    palette = b''.join(struct.pack('<BBBB', c & 0xff, c >> 8 & 0xff, c >> 16, 0) for c in GRAPHITE)
    offset = 14 + 40 + len(palette)
    bmp = (struct.pack('<2sIHHI', b'BM', offset + len(pixels), 0, 0, offset) +
           struct.pack('<IiiHHIIiiII', 40, 32, 32, 1, 4, 0, len(pixels), 2835, 2835, 16, 0) +
           palette + pixels)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'icon.bmp')
    with open(out, 'wb') as f:
        f.write(bmp)
    print(out)


if __name__ == '__main__':
    main()
