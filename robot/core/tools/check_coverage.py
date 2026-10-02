#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fail when lcov --summary output reports line coverage below the threshold."""
import argparse
import re
import sys

ap = argparse.ArgumentParser()
ap.add_argument("summary")
ap.add_argument("--lines", type=float, required=True)
a = ap.parse_args()
text = open(a.summary).read()
m = re.search(r"lines\.*:\s*([\d.]+)%", text)
if not m:
    sys.exit("no line coverage found in " + a.summary)
pct = float(m.group(1))
print(f"line coverage {pct:.1f}% (required {a.lines:.0f}%)")
sys.exit(0 if pct >= a.lines else 1)
