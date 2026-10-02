#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate robot/hardware/pinmap.yaml. Exit status 1 on any rule violation."""
import os
import re
import sys

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
PIN_RE = re.compile(r"^P([12])\.(0[1-9]|[12][0-9]|3[0-6])$")
OWNERS = {"PRU0", "PRU1", "M4F", "A53"}
DIRS = {"in", "out", "io"}
STATUS = {"tbd", "candidate", "verified"}


def load(path):
    with open(path) as f:
        return yaml.safe_load(f)


def check(pinmap, reserved):
    """Return a list of human-readable problems (empty list = OK)."""
    errs = []
    used = {}
    seen = set()
    reserved_pins = set(reserved.get("gamepup", [])) | set(reserved.get("fixed", []))
    for sig in pinmap.get("signals", []):
        n = sig.get("name")
        if not n or n in seen:
            errs.append(f"duplicate or missing signal name: {n!r}")
        seen.add(n)
        if sig.get("owner") not in OWNERS:
            errs.append(f"{n}: owner must be one of {sorted(OWNERS)}")
        if sig.get("dir") not in DIRS:
            errs.append(f"{n}: dir must be one of {sorted(DIRS)}")
        st = sig.get("status")
        if st not in STATUS:
            errs.append(f"{n}: status must be one of {sorted(STATUS)}")
        pin = sig.get("header_pin")
        if pin is None:
            if st != "tbd":
                errs.append(f"{n}: status {st} requires header_pin")
            continue
        if not PIN_RE.match(str(pin)):
            errs.append(f"{n}: bad header pin {pin!r} (expected P1.01..P2.36)")
            continue
        if pin in used:
            errs.append(f"{n}: header pin {pin} already used by {used[pin]}")
        used[pin] = n
        if pin in reserved_pins and not sig.get("gamepup_conflict_ok"):
            errs.append(f"{n}: header pin {pin} is used by the GamePup overlays/fixed function")
        if st == "verified" and not (sig.get("pad") and sig.get("mux")):
            errs.append(f"{n}: verified requires pad and mux")
    return errs


def main(argv):
    base = os.path.join(HERE, "..")
    pinmap = load(os.path.join(base, "pinmap.yaml"))
    reserved = load(os.path.join(base, "reserved_pins.yaml"))
    errs = check(pinmap, reserved)
    sigs = pinmap["signals"]
    done = sum(1 for s in sigs if s.get("header_pin"))
    print(f"pinmap: {len(sigs)} signals, {done} assigned, "
          f"{sum(1 for s in sigs if s.get('status') == 'verified')} verified")
    for e in errs:
        print("ERROR:", e)
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
