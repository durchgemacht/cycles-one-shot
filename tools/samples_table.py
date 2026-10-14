#!/usr/bin/env python3
"""Take the interpolation table and the decimator of the Model:Samples OS 1.13 from your own OS file.

The Model:Samples plays samples through a 6-tap polyphase filter with 256 phases at twice the
output rate (inner loop at 0x400a7082 in OS 1.13). Its taps sit in SRAM at 0x8000a000 as three
banks of 1 KB, each holding two taps per phase; the reset code copies them there from the
MAIN OS at 0x401A44F0. A 2:1 decimator follows (0x400a714e), the same code with the same four
coefficients as the Cycles' 0x400a9f58; they are immediates of its first instructions.

This script reads both from your copy of the official OS file and writes
  build/samples_table.bin      256 rows of 6 big-endian int16, the row layout of the core
                               (tests/measure --samples, tools/render --table)
  build/samples_decimator.bin  the 4 coefficients as big-endian Q31 int32 (tools/render)
Nothing of it belongs in the repository.

    python3 tools/samples_table.py --stock model-samples_OS1.13.syx --tool PATH/elektron-firmware-tool
    python3 tools/samples_table.py --main-os section_3_MAIN_OS.bin     (already unpacked)

elektron-firmware-tool: MIT, Marcel Bierling, https://github.com/mischa85/elektron-firmware-tool
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
STOCK_SHA256 = "e11859b68deb7e5e3fe86ab32581212093849c4be5d3950add011eac398a2ce8"     # model-samples_OS1.13.syx
MAIN_OS_SHA256 = "a351392c62ec1c6c3324a807baf46934690d54edfc76029a4b4882541cad1ab2"   # its section 3, unpacked
TOOL_NAME = "elektron-firmware-tool"
TOOL_URL = "https://github.com/mischa85/elektron-firmware-tool"

MAIN_OS = 3
BASE = 0x40000400                   # where the MAIN OS runs
TABLE = 0x401A44F0                  # copied to 0x8000a000 at reset
BANK = 1024
PHASES, TAPS = 256, 6

# The code the address rests on: (address, stock bytes, what).
CHECKS = [
    (0x40000486, "45f9 401a 24f0", "reset: lea 0x401a24f0,%a2 (source of the copy)"),
    (0x4000048c, "43f9 8000 8000", "reset: lea 0x80008000,%a1 (SRAM destination)"),
    (0x400a7038, "43f9 8000 a000", "voice: lea 0x8000a000,%a1 (bank 0, taps 0-1)"),
    (0x400a7086, "aead 60ce 0400", "voice: macw ... %a5@(1024) (bank 1, taps 2-3)"),
    (0x400a709a, "aead e007 0800", "voice: macw ... %a5@(2048) (bank 2, taps 4-5)"),
    (0x400a71a0, "2a3c 7fff ffff", "decimator: movel #0x7fffffff,%d5"),
]

# Decimator coefficients: movel #imm,%d0..%d3, the immediate 2 bytes in.
DECIMATOR = [(0x400a7182, "203c"), (0x400a7188, "223c"), (0x400a718e, "243c"), (0x400a7194, "263c")]


def find_tool(path):
    cand = path or os.environ.get("ELEKTRON_FIRMWARE_TOOL") or shutil.which(TOOL_NAME)
    if not cand or not Path(cand).is_file() or not os.access(cand, os.X_OK):
        sys.exit("%s not found: build it from %s (make), then pass --tool PATH, set "
                 "ELEKTRON_FIRMWARE_TOOL, or put it on PATH" % (TOOL_NAME, TOOL_URL))
    return Path(cand)


def unpack(stock, tool):
    if hashlib.sha256(Path(stock).read_bytes()).hexdigest() != STOCK_SHA256:
        sys.exit("%s is not the official Model:Samples OS 1.13 file (SHA-256 %s...)" % (stock, STOCK_SHA256[:16]))
    with tempfile.TemporaryDirectory() as tmp:
        res = subprocess.run([str(tool), "-i", str(stock), "-d", str(MAIN_OS), "-o", tmp],
                             capture_output=True, text=True)
        if res.returncode != 0:
            sys.exit("%s failed:\n%s%s" % (TOOL_NAME, res.stdout, res.stderr))
        return (Path(tmp) / "section_3_MAIN_OS.bin").read_bytes()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--stock", type=Path, help="your official model-samples_OS1.13.syx")
    src.add_argument("--main-os", type=Path, help="its section 3, already unpacked")
    ap.add_argument("--tool", help="path to elektron-firmware-tool (with --stock)")
    ap.add_argument("--out", type=Path, default=HERE / "build" / "samples_table.bin")
    ap.add_argument("--decimator-out", type=Path, default=HERE / "build" / "samples_decimator.bin")
    args = ap.parse_args()

    image = unpack(args.stock, find_tool(args.tool)) if args.stock else args.main_os.read_bytes()
    if hashlib.sha256(image).hexdigest() != MAIN_OS_SHA256:
        sys.exit("not the MAIN OS of the official Model:Samples OS 1.13 (SHA-256 %s...)" % MAIN_OS_SHA256[:16])
    for addr, stock, what in CHECKS:
        want = bytes.fromhex(stock.replace(" ", ""))
        if image[addr - BASE:addr - BASE + len(want)] != want:
            sys.exit("unexpected bytes at 0x%08x (%s)" % (addr, what))

    rows = []
    for p in range(PHASES):
        taps = []
        for bank in range(3):
            off = TABLE - BASE + bank * BANK + 4 * p
            taps += struct.unpack(">hh", image[off:off + 4])
        rows.append(taps)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(b"".join(struct.pack(">%dh" % TAPS, *r) for r in rows))
    sums = [sum(r) for r in rows]
    print("%s: %d rows of %d taps, row sums %d..%d" % (args.out, PHASES, TAPS, min(sums), max(sums)))

    coefs = []
    for addr, opcode in DECIMATOR:
        if image[addr - BASE:addr - BASE + 2] != bytes.fromhex(opcode):
            sys.exit("unexpected bytes at 0x%08x (decimator coefficient)" % addr)
        coefs.append(image[addr - BASE + 2:addr - BASE + 6])
    args.decimator_out.write_bytes(b"".join(coefs))
    print("%s: %s" % (args.decimator_out, " ".join("%.6f" % (struct.unpack(">i", c)[0] / 2 ** 31) for c in coefs)))


if __name__ == "__main__":
    main()
