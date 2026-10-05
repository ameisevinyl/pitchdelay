#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 ameisevinyl
"""Write a 5 s, 48 kHz mono 32-bit-float WAV with a single impulse at frame 48000."""
import struct
import sys

RATE, SECONDS, IMPULSE_AT = 48000, 5, 48000


def main(path):
    frames = RATE * SECONDS
    data = bytearray(4 * frames)
    struct.pack_into("<f", data, 4 * IMPULSE_AT, 1.0)
    fmt = struct.pack("<HHIIHH", 3, 1, RATE, RATE * 4, 4, 32)   # 3 = IEEE float
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt \
        + b"data" + struct.pack("<I", len(data)) + bytes(data)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)
    print(f"wrote {path}: impulse at frame {IMPULSE_AT}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: make_impulse.py out.wav")
    main(sys.argv[1])
