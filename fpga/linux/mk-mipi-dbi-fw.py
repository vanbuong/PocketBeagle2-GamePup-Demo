#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build a panel-mipi-dbi firmware blob from a text command list.

Same file format as the kernel's "mipi-dbi-cmd" helper: 15-byte magic, version 1,
then  command, num_params, params...  with `delay N` encoded as 0x00 0x01 N.

  mk-mipi-dbi-fw.py INPUT.txt OUTPUT.bin      build
  mk-mipi-dbi-fw.py -d FILE.bin               dump
"""
import sys

MAGIC = b"MIPI DBI" + bytes(7)


def build(text):
    out = bytearray(MAGIC)
    out.append(1)
    for n, line in enumerate(text.splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        kw, *args = line.split()
        try:
            vals = [int(a, 0) for a in args]
        except ValueError:
            raise SystemExit("line %d: not a number: %s" % (n, line))
        if any(v < 0 or v > 255 for v in vals):
            raise SystemExit("line %d: value out of range: %s" % (n, line))
        if kw == "command" and vals:
            out += bytes([vals[0], len(vals) - 1]) + bytes(vals[1:])
        elif kw == "delay" and len(vals) == 1:
            out += bytes([0x00, 1, vals[0]])
        else:
            raise SystemExit("line %d: bad statement: %s" % (n, line))
    return bytes(out)


def dump(buf):
    if len(buf) < 16 or buf[:15] != MAGIC or buf[15] != 1:
        raise SystemExit("not a MIPI DBI v1 file")
    i, lines = 16, []
    while i < len(buf):
        cmd, n = buf[i], buf[i + 1]
        params = buf[i + 2:i + 2 + n]
        if len(params) != n:
            raise SystemExit("truncated at offset %d" % i)
        lines.append("delay %d" % params[0] if cmd == 0 and n == 1 else
                     " ".join(["command 0x%02x" % cmd] + ["0x%02x" % p for p in params]))
        i += 2 + n
    return "\n".join(lines)


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "-d":
        print(dump(open(a[1], "rb").read()))
    elif len(a) == 2:
        open(a[1], "wb").write(build(open(a[0]).read()))
    else:
        raise SystemExit(__doc__)
