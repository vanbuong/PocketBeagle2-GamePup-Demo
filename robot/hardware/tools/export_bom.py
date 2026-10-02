#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Write a grouped BOM (CSV) from a KiCad schematic, ignoring power symbols/flags.

Usage: export_bom.py in.kicad_sch [out.csv]
"""
import csv
import re
import sys
from collections import defaultdict

from kiutils.schematic import Schematic


def natural(ref):
    m = re.match(r"([A-Za-z_#]+)(\d+)", ref)
    return (m.group(1), int(m.group(2))) if m else (ref, 0)


def bom_rows(path):
    sch = Schematic.from_file(path)
    groups = defaultdict(set)
    for s in sch.schematicSymbols:
        props = {p.key: p.value for p in s.properties}
        ref = props.get("Reference", "")
        if ref.startswith("#"):
            continue
        groups[(props.get("Value", ""), props.get("Footprint", ""))].add(ref)
    rows = []
    for (value, fp), refs in groups.items():
        refs = sorted(refs, key=natural)
        rows.append({"Qty": len(refs), "Value": value, "Footprint": fp, "References": " ".join(refs)})
    rows.sort(key=lambda r: natural(r["References"].split()[0]))
    return rows


def main(argv):
    rows = bom_rows(argv[0])
    out = open(argv[1], "w", newline="") if len(argv) > 1 else sys.stdout
    w = csv.DictWriter(out, fieldnames=["Qty", "Value", "Footprint", "References"], lineterminator="\n")
    w.writeheader()
    w.writerows(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
