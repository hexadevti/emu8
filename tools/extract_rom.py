#!/usr/bin/env python3
"""Extract an embedded C/C++ byte-array ROM into a raw .bin file.

Used by the "ROMs off flash -> onto SD" refactor: every platform that still
embeds a ROM as a `const unsigned char foo[] = { 0x.., ... };` array gets that
array dumped to a binary so it can live on the SD card under /roms/<platform>/.

Usage:
    python tools/extract_rom.py <source-file> <symbol> <out.bin> [--expect N]

Parses the first `<symbol>[ ... ] = { ... };` initializer it finds and writes
every integer byte token (0x.. hex or decimal) in order. With --expect N the
output length is asserted to be exactly N bytes.
"""
import re
import sys
import argparse


def extract(path: str, symbol: str) -> bytes:
    text = open(path, "r", encoding="utf-8", errors="replace").read()
    # Find "<symbol> [ optional-size ] = {"  (the array initializer opening brace).
    m = re.search(r"\b" + re.escape(symbol) + r"\s*\[[^\]]*\]\s*=\s*\{", text)
    if not m:
        raise SystemExit(f"error: symbol '{symbol}' (array initializer) not found in {path}")
    start = m.end()
    # Walk to the matching closing brace of this single initializer.
    depth = 1
    i = start
    while i < len(text) and depth:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    body = text[start : i - 1]
    # Strip block + line comments so commented-out bytes don't sneak in.
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    out = bytearray()
    for tok in re.findall(r"0[xX][0-9a-fA-F]+|\d+", body):
        out.append(int(tok, 0) & 0xFF)
    return bytes(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("symbol")
    ap.add_argument("out")
    ap.add_argument("--expect", type=int, default=None)
    args = ap.parse_args()
    data = extract(args.source, args.symbol)
    if args.expect is not None and len(data) != args.expect:
        raise SystemExit(f"error: {args.symbol} extracted {len(data)} bytes, expected {args.expect}")
    open(args.out, "wb").write(data)
    print(f"{args.symbol}: {len(data)} bytes -> {args.out}")


if __name__ == "__main__":
    main()
