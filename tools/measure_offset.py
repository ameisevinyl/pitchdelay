#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 ameisevinyl
"""Find the impulse (largest |sample|, channel 0) in two WAVs and print the frame offset."""
import argparse
import struct
import sys


def read_channel0(path):
    raw = open(path, "rb").read()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        sys.exit(f"{path}: not a RIFF/WAVE file")
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(raw):
        cid, size = raw[pos:pos + 4], struct.unpack("<I", raw[pos + 4:pos + 8])[0]
        body = raw[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, rate, _, align, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE:                       # WAVE_FORMAT_EXTENSIBLE
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, ch, rate, align, bits)
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    if fmt is None or pcm is None:
        sys.exit(f"{path}: missing fmt or data chunk")
    tag, ch, rate, align, bits = fmt
    n = len(pcm) // align
    out = []
    for i in range(n):
        o = i * align
        if tag == 3 and bits == 32:
            out.append(struct.unpack_from("<f", pcm, o)[0])
        elif tag == 1 and bits == 16:
            out.append(struct.unpack_from("<h", pcm, o)[0] / 32768.0)
        elif tag == 1 and bits == 24:
            out.append(int.from_bytes(pcm[o:o + 3], "little", signed=True) / 8388608.0)
        elif tag == 1 and bits == 32:
            out.append(struct.unpack_from("<i", pcm, o)[0] / 2147483648.0)
        else:
            sys.exit(f"{path}: unsupported format tag={tag} bits={bits}")
    return out, rate


def peak(samples):
    best, idx = 0.0, -1
    for i, v in enumerate(samples):
        if abs(v) > best:
            best, idx = abs(v), i
    return idx, best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("delayed")
    ap.add_argument("--expect", type=int, help="expected offset in frames")
    a = ap.parse_args()
    ref, r1 = read_channel0(a.reference)
    dly, r2 = read_channel0(a.delayed)
    (i1, p1), (i2, p2) = peak(ref), peak(dly)
    print(f"reference: peak {p1:.6f} at frame {i1} ({r1} Hz)")
    print(f"delayed:   peak {p2:.6f} at frame {i2} ({r2} Hz)")
    offset = i2 - i1
    print(f"offset: {offset} frames")
    if a.expect is not None:
        ok = offset == a.expect
        print("PASS" if ok else f"FAIL (expected {a.expect})")
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
