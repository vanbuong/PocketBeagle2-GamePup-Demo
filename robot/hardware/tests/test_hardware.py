# SPDX-License-Identifier: GPL-2.0-only
"""Consistency tests for the robot hardware sources (no KiCad installation needed)."""
import copy
import os
import re
import sys

import pytest
import yaml

HW = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
REPO = os.path.abspath(os.path.join(HW, "..", ".."))
sys.path.insert(0, os.path.join(HW, "tools"))

import check_pinmap  # noqa: E402
import export_bom  # noqa: E402
import gen_rev0  # noqa: E402

SCH = os.path.join(HW, "rev0", "balbot_rev0.kicad_sch")


@pytest.fixture(scope="module")
def pinmap():
    return check_pinmap.load(os.path.join(HW, "pinmap.yaml"))


@pytest.fixture(scope="module")
def reserved():
    return check_pinmap.load(os.path.join(HW, "reserved_pins.yaml"))


def test_pinmap_is_valid(pinmap, reserved):
    assert check_pinmap.check(pinmap, reserved) == []


def test_pinmap_order_matches_schematic_connector(pinmap):
    assert [s["name"] for s in pinmap["signals"]] == gen_rev0.SIGNALS


def test_schematic_is_up_to_date(tmp_path):
    out = tmp_path / "regen.kicad_sch"
    gen_rev0.build(str(out))
    assert out.read_bytes() == open(SCH, "rb").read(), \
        "rev0 schematic is stale: run tools/gen_rev0.py and commit the result"


def test_every_signal_is_labelled_on_connector_and_module(pinmap):
    text = open(SCH).read()
    for s in pinmap["signals"]:
        n = len(re.findall(r'\(label "%s"' % re.escape(s["name"]), text))
        assert n >= 2, f"{s['name']} appears only {n} time(s) as a net label"


def test_bom_matches_committed_file():
    rows = export_bom.bom_rows(SCH)
    refs = " ".join(r["References"] for r in rows).split()
    assert len(refs) == len(set(refs)), "duplicate reference designators"
    for needed in ("U1", "J18", "J7", "J8", "J14", "J9", "J10", "F1", "C1", "D1"):
        assert needed in refs
    assert not any(r.startswith("#") for r in refs)
    with open(os.path.join(HW, "rev0", "BOM.csv")) as f:
        committed = f.read()
    import io
    buf = io.StringIO()
    import csv
    w = csv.DictWriter(buf, fieldnames=["Qty", "Value", "Footprint", "References"], lineterminator="\n")
    w.writeheader()
    w.writerows(rows)
    assert buf.getvalue() == committed, "BOM.csv is stale: run tools/export_bom.py"


def test_reserved_pins_cover_gamepup_overlays(reserved):
    found = set()
    odir = os.path.join(REPO, "overlays")
    for fn in os.listdir(odir):
        if fn.endswith(".dts"):
            for m in re.finditer(r"\bP([12])\.(\d\d)", open(os.path.join(odir, fn)).read()):
                found.add(f"P{m.group(1)}.{m.group(2)}")
    assert found, "no pins found in overlays (regex or path wrong)"
    missing = found - set(reserved["gamepup"])
    assert not missing, f"overlay pins missing from reserved_pins.yaml: {sorted(missing)}"


# ---- rules of check_pinmap, on synthetic data --------------------------------------

def _pm(**kw):
    sig = {"name": "X", "dir": "in", "owner": "PRU0", "header_pin": None,
           "pad": None, "mux": None, "status": "tbd"}
    sig.update(kw)
    return {"signals": [sig]}


RES = {"gamepup": ["P1.02"], "fixed": []}


def test_rule_unassigned_ok():
    assert check_pinmap.check(_pm(), RES) == []


@pytest.mark.parametrize("pin", ["P3.01", "P1.00", "P1.37", "1.05", "P1.5"])
def test_rule_bad_pin_format(pin):
    assert any("bad header pin" in e for e in check_pinmap.check(_pm(header_pin=pin, status="candidate"), RES))


def test_rule_reserved_conflict_and_override():
    assert any("GamePup" in e for e in check_pinmap.check(_pm(header_pin="P1.02", status="candidate"), RES))
    assert check_pinmap.check(_pm(header_pin="P1.02", status="candidate", gamepup_conflict_ok=True), RES) == []


def test_rule_duplicate_pin():
    pm = _pm(header_pin="P2.12", status="candidate")
    second = copy.deepcopy(pm["signals"][0])
    second["name"] = "Y"
    pm["signals"].append(second)
    assert any("already used" in e for e in check_pinmap.check(pm, RES))


def test_rule_verified_needs_pad_and_mux():
    assert any("verified requires" in e for e in check_pinmap.check(_pm(header_pin="P2.12", status="verified"), RES))
    assert check_pinmap.check(_pm(header_pin="P2.12", status="verified", pad="AA1", mux="PRU0_R31_3"), RES) == []


def test_rule_assigned_pin_requires_non_tbd_status():
    assert any("requires header_pin" in e for e in check_pinmap.check(_pm(status="candidate"), RES))


def test_rule_bad_owner_dir_status():
    errs = check_pinmap.check(_pm(owner="DSP", dir="x", status="maybe"), RES)
    assert len(errs) >= 3


# ---- connectivity, via KiCad's own netlist export (skipped when kicad-cli is absent) ----

import shutil  # noqa: E402
import subprocess  # noqa: E402


def _netlist(tmp_path):
    out = tmp_path / "rev0.net"
    subprocess.run(["kicad-cli", "sch", "export", "netlist", "--format", "kicadsexpr",
                    "--output", str(out), SCH], check=True, capture_output=True)
    nets = {}
    for part in out.read_text().split("(net (code")[1:]:
        name = re.search(r'\(name "([^"]*)"\)', part).group(1).lstrip("/")
        nets[name] = set(re.findall(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)', part))
    return nets


@pytest.mark.skipif(not shutil.which("kicad-cli"), reason="kicad-cli not installed")
def test_netlist_safety_and_signal_paths(tmp_path):
    n = _netlist(tmp_path)
    # STBY = PRU_EN & M4F_EN & ESTOP_OK through both gates of U1, pulled low by R13
    assert {("U1", "1")} <= n["MOT_EN_PRU"] and {("U1", "2")} <= n["MOT_EN_M4F"]
    assert ("U1", "6") in n["ESTOP_OK"] and ("J11", "2") in n["ESTOP_OK"] and ("R14", "1") in n["ESTOP_OK"]
    assert {("U1", "5"), ("U1", "7")} == n["GATE_X"]
    assert ("U1", "3") in n["TB_STBY"] and ("J7", "6") in n["TB_STBY"] and ("R13", "1") in n["TB_STBY"]
    # every TB6612 input has a series resistor from the PB2 side and a pull-down
    for sig, tb, pin in (("MOT_L_PWM", "TB_PWMA", "3"), ("MOT_L_IN1", "TB_AIN1", "4"), ("MOT_L_IN2", "TB_AIN2", "5"),
                         ("MOT_R_PWM", "TB_PWMB", "9"), ("MOT_R_IN1", "TB_BIN1", "7"), ("MOT_R_IN2", "TB_BIN2", "8")):
        assert ("J7", pin) in n[tb] and ("J18", str(gen_rev0.SIGNALS.index(sig) + 1)) in n[sig]
        assert len(n[tb]) == 3 and len(n[sig]) == 2
    # motor outputs reach both the driver and the motor connector
    assert {("J8", "3"), ("J9", "1")} <= n["ML_P"] and {("J8", "5"), ("J10", "1")} <= n["MR_P"]
    # encoders: connector -> RC filter -> 0R -> J18
    for side, j in (("L", "J9"), ("R", "J10")):
        for ch, pin in (("A", "3"), ("B", "4")):
            assert (j, pin) in n[f"ENC_{side}_{ch}_RAW"]
            assert ("J18", str(gen_rev0.SIGNALS.index(f"ENC_{side}_{ch}") + 1)) in n[f"ENC_{side}_{ch}"]
    # power rails and ground reach their modules
    assert {("J8", "1"), ("C1", "1"), ("D1", "1"), ("J3", "1"), ("J16", "2")} <= n["+BATT"]
    assert {("J3", "3"), ("J4", "1"), ("J5", "1"), ("J6", "1")} <= n["+5V"]
    assert {("J4", "3"), ("J7", "1"), ("J14", "1"), ("J9", "5"), ("J10", "5")} <= n["+3V3"]
    assert {("J1", "2"), ("J8", "2"), ("U1", "4")} <= n["GND"]
    # no net may join the battery rail to 5 V, 3.3 V or ground
    assert n["+BATT"].isdisjoint(n["+5V"] | n["+3V3"] | n["GND"])
    assert n["+5V"].isdisjoint(n["+3V3"] | n["GND"]) and n["+3V3"].isdisjoint(n["GND"])
