#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate the Rev 0 module-interconnect schematic (KiCad 7).

Rev 0 is the perfboard / breadboard build described in docs/balance-robot/09-pcb-design.md:
off-the-shelf modules (TB6612, MPU-6500, INA260, buck, LDO) wired together with a small
amount of glue (pull-downs, filters, the TB6612 STBY gate). The schematic is generated so
that it is reproducible and diffable; the PocketBeagle 2 header pins are NOT drawn here:
all logical signals leave through connector J_SIG and the header-pin assignment lives in
robot/hardware/pinmap.yaml (to be filled in during milestone M0).

Usage: gen_rev0.py [output.kicad_sch]
Requires: kiutils, KiCad 7 stock symbol libraries in /usr/share/kicad/symbols.
"""
import math
import os
import sys
import uuid

from kiutils.items.common import Effects, Font, Justify, PageSettings, Position, Property, Stroke
from kiutils.items.schitems import (Connection, HierarchicalSheetInstance, LocalLabel,
                                    SchematicSymbol, SymbolProjectInstance, SymbolProjectPath,
                                    Text)
from kiutils.schematic import Schematic
from kiutils.symbol import SymbolLib

SYMDIR = os.environ.get("KICAD_SYMBOL_DIR", "/usr/share/kicad/symbols")
PROJECT = "balbot_rev0"
POWER = {"GND", "+3V3", "+5V", "+BATT"}

FP_HDR = "Connector_PinHeader_2.54mm:PinHeader_1x{n:02d}_P2.54mm_Vertical"
FP_R = "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal"
FP_C = "Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm"
FP_CP = "Capacitor_THT:CP_Radial_D10.0mm_P5.00mm"
FP_TVS = "Diode_THT:D_DO-201AD_P15.24mm_Horizontal"
FP_FUSE = "Fuse:Fuseholder_Blade_Mini_Keystone_3568"
FP_XT30 = "Connector_AMASS:AMASS_XT30PW-M_1x02_P2.50mm_Horizontal"
FP_PH6 = "Connector_JST:JST_PH_B6B-PH-K_1x06_P2.00mm_Vertical"
FP_U1 = "Package_SO:VSSOP-8_2.3x2mm_P0.5mm"

# Logical signals that leave through J_SIG (order = J_SIG pin order, mirrored in pinmap.yaml).
SIGNALS = [
    "ENC_L_A", "ENC_L_B", "ENC_R_A", "ENC_R_B",
    "MOT_L_PWM", "MOT_L_IN1", "MOT_L_IN2", "MOT_R_PWM", "MOT_R_IN1", "MOT_R_IN2",
    "MOT_EN_PRU", "MOT_EN_M4F", "ESTOP_OK", "ARM_BTN",
    "IMU_SCLK", "IMU_MOSI", "IMU_MISO", "IMU_CS", "IMU_INT",
    "I2C_SCL", "I2C_SDA", "INA_ALERT", "UART_TX", "UART_RX",
]


def r(v):
    return round(v + 0.0, 3)


_UID_COUNTER = [0]


def uid():
    """Deterministic UUIDs so regenerating the schematic gives a byte-identical file."""
    _UID_COUNTER[0] += 1
    return str(uuid.uuid5(uuid.NAMESPACE_URL, f"balbot-rev0/{_UID_COUNTER[0]}"))


class Lib:
    def __init__(self):
        self.files = {}
        self.cache = {}

    def get(self, nick, name):
        key = (nick, name)
        if key not in self.cache:
            if nick not in self.files:
                self.files[nick] = SymbolLib.from_file(os.path.join(SYMDIR, nick + ".kicad_sym"))
            sym = next(s for s in self.files[nick].symbols if s.entryName == name)
            assert sym.extends is None, f"{nick}:{name} uses extends"
            sym.libraryNickname = nick
            self.cache[key] = sym
        return self.cache[key]


class Gen:
    def __init__(self):
        self.lib = Lib()
        self.sch = Schematic(version="20230121", generator="eeschema", uuid=uid(),
                             paper=PageSettings(paperSize="A1"))
        self.used_syms = {}
        self.counts = {}
        self.refs = set()
        self.col_x = 0.0
        self.cur_y = 0.0
        self.col_w = 0.0

    # -- layout -------------------------------------------------------------
    def column(self, x, title):
        # x is ignored: sections are packed left to right after the previous one
        x = getattr(self, "next_x", 20.0)
        self.col_x = x
        self.max_x = x
        self.cur_y = 40.0
        self.next_x = x + 70.0
        self.text(title, x, 28.0, size=2.0)

    def text(self, s, x, y, size=1.27):
        self.sch.texts.append(Text(text=s, position=Position(X=r(x), Y=r(y), angle=0),
                                   effects=Effects(font=Font(height=size, width=size),
                                                   justify=Justify(horizontally="left")),
                                   uuid=uid()))

    # -- parts --------------------------------------------------------------
    def add(self, nick, name, ref_prefix, value, footprint, nets, unit=1, note=None,
            flags=(), ref=None, gap=7.62, dx=0.0, xoff=None):
        """Place one symbol (one unit) and attach every pin to a net.

        nets: {pin number or pin name: net name}. Net names in POWER get a power symbol,
        everything else a local label. flags: net names that also get a PWR_FLAG here.
        """
        if self.cur_y > 470.0:  # wrap long sections into a second sub-column
            self.col_x += 60.0
            self.cur_y = 40.0
        self.max_x = max(self.max_x, self.col_x)
        self.next_x = max(self.next_x, self.max_x + 70.0)
        sym = self.lib.get(nick, name)
        if (nick, name) not in self.used_syms:
            self.used_syms[(nick, name)] = sym
        pins = [p for u in sym.units if u.unitId in (0, unit) for p in u.pins]
        ys = [p.position.Y for p in pins]
        xs = [p.position.X for p in pins]
        height = (max(ys) - min(ys)) if ys else 0
        top = max(ys)
        # origin so the symbol's top pin sits at cur_y + 6
        px = self.col_x + (xoff if xoff is not None else 14.0) + dx - min(xs)
        py = self.cur_y + 6.0 + top
        px, py = r(px), r(py)
        self.cur_y = r(py + (-min(ys)) + gap + 6.0)
        if ref is None:
            n = self.counts.get(ref_prefix, 0) + 1
            while f"{ref_prefix}{n}" in self.refs:
                n += 1
            self.counts[ref_prefix] = n
            ref = f"{ref_prefix}{n}"
        elif unit == 1 or ref not in self.refs:
            assert ref not in self.refs, f"duplicate ref {ref}"
        self.refs.add(ref)
        sp = SchematicSymbol(
            libraryNickname=nick, entryName=name,
            position=Position(X=px, Y=py, angle=0), unit=unit, inBom=True, onBoard=True,
            fieldsAutoplaced=False, uuid=uid(),
            properties=[
                Property(key="Reference", value=ref, id=0,
                         position=Position(X=r(px + 3), Y=r(py - top - 3), angle=0),
                         effects=Effects(font=Font(height=1.27, width=1.27),
                                         justify=Justify(horizontally="left"))),
                Property(key="Value", value=value, id=1,
                         position=Position(X=r(px + 3), Y=r(py - top - 1), angle=0),
                         effects=Effects(font=Font(height=1.27, width=1.27),
                                         justify=Justify(horizontally="left"))),
                Property(key="Footprint", value=footprint, id=2,
                         position=Position(X=px, Y=py, angle=0),
                         effects=Effects(font=Font(height=1.27, width=1.27), hide=True)),
                Property(key="Datasheet", value="~", id=3,
                         position=Position(X=px, Y=py, angle=0),
                         effects=Effects(font=Font(height=1.27, width=1.27), hide=True)),
            ],
            pins={p.number: uid() for p in pins},
            instances=[SymbolProjectInstance(
                name=PROJECT,
                paths=[SymbolProjectPath(sheetInstancePath="/" + self.sch.uuid,
                                         reference=ref, unit=unit)])],
        )
        self.sch.schematicSymbols.append(sp)
        if note:
            self.text(note, px + 3, r(py - top + 1.0 + 0.0), size=1.0)
        for p in pins:
            key = p.number if p.number in nets else p.name
            if key not in nets:
                raise KeyError(f"{ref}: no net for pin {p.number}/{p.name}")
            net = nets[key]
            ex, ey = r(px + p.position.X), r(py - p.position.Y)
            self.attach(ex, ey, p.position.angle, net, flag=net in flags)
        return ref

    def attach(self, ex, ey, pin_angle, net, flag=False):
        ang = math.radians(pin_angle + 180.0)
        dxo, dyo = round(math.cos(ang)), round(-math.sin(ang))  # outward, screen coords
        L = 2.54
        sx, sy = r(ex + dxo * L), r(ey + dyo * L)
        self.sch.graphicalItems.append(Connection(
            type="wire", points=[Position(X=ex, Y=ey), Position(X=sx, Y=sy)],
            stroke=Stroke(width=0, type="default"), uuid=uid()))
        if net in POWER:
            self.power(net, sx, sy, flag)
        else:
            if (dxo, dyo) == (1, 0):
                a, j = 0, "left"
            elif (dxo, dyo) == (-1, 0):
                a, j = 180, "right"
            elif (dxo, dyo) == (0, 1):
                a, j = 270, "right"
            else:
                a, j = 90, "left"
            self.sch.labels.append(LocalLabel(
                text=net, position=Position(X=sx, Y=sy, angle=a),
                effects=Effects(font=Font(height=1.27, width=1.27),
                                justify=Justify(horizontally=j)), uuid=uid()))

    def power(self, net, x, y, flag):
        for name in ([net] + (["PWR_FLAG"] if flag else [])):
            sym = self.lib.get("power", name)
            self.used_syms[("power", name)] = sym
            n = self.counts.get("#PWR", 0) + 1
            self.counts["#PWR"] = n
            ref = f"#FLG0{n}" if name == "PWR_FLAG" else f"#PWR0{n}"
            self.sch.schematicSymbols.append(SchematicSymbol(
                libraryNickname="power", entryName=name,
                position=Position(X=x, Y=y, angle=0), unit=1, inBom=False, onBoard=False,
                fieldsAutoplaced=False, uuid=uid(),
                properties=[
                    Property(key="Reference", value=ref, id=0, position=Position(X=x, Y=r(y - 3), angle=0),
                             effects=Effects(font=Font(height=1.27, width=1.27), hide=True)),
                    Property(key="Value", value=name, id=1, position=Position(X=x, Y=r(y - 2), angle=0),
                             effects=Effects(font=Font(height=1.27, width=1.27))),
                    Property(key="Footprint", value="", id=2, position=Position(X=x, Y=y, angle=0),
                             effects=Effects(font=Font(height=1.27, width=1.27), hide=True)),
                    Property(key="Datasheet", value="", id=3, position=Position(X=x, Y=y, angle=0),
                             effects=Effects(font=Font(height=1.27, width=1.27), hide=True)),
                ],
                pins={"1": uid()},
                instances=[SymbolProjectInstance(
                    name=PROJECT,
                    paths=[SymbolProjectPath(sheetInstancePath="/" + self.sch.uuid,
                                             reference=ref, unit=1)])]))

    # -- convenience wrappers ------------------------------------------------
    def hdr(self, ref, value, nets, note=None, fp=None, **kw):
        n = len(nets)
        nm = {str(i + 1): v for i, v in enumerate(nets)}
        return self.add("Connector_Generic", f"Conn_01x{n:02d}", "J", value,
                        fp or FP_HDR.format(n=n), nm, ref=ref, note=note, **kw)

    def res(self, value, a, b, ref=None, note=None, **kw):
        return self.add("Device", "R", "R", value, FP_R, {"1": a, "2": b}, ref=ref, note=note, **kw)

    def cap(self, value, a, b, ref=None, polar=False, **kw):
        if polar:
            return self.add("Device", "C_Polarized", "C", value, FP_CP, {"1": a, "2": b}, ref=ref, **kw)
        return self.add("Device", "C", "C", value, FP_C, {"1": a, "2": b}, ref=ref, **kw)

    def finish(self, path):
        for (nick, name), sym in self.used_syms.items():
            self.sch.libSymbols.append(sym)
        self.sch.sheetInstances = [HierarchicalSheetInstance(instancePath="/", page="1")]
        self.sch.to_file(path)


def build(path):
    _UID_COUNTER[0] = 0
    g = Gen()

    # ---------------- column 1: power --------------------------------------
    g.column(20, "1. POWER (3S 18650, BMS, fuse, switch)")
    g.hdr("J1", "XT30 battery (from 3S BMS)", ["BAT_RAW", "GND"], fp=FP_XT30,
          note="Pin1 = pack +, pin2 = pack -. 3S BMS sits between cells and J1.", flags=("GND",))
    g.add("Device", "Fuse", "F", "5A blade", FP_FUSE, {"1": "BAT_RAW", "2": "BAT_FUSED"})
    g.hdr("J2", "Main power switch (rocker)", ["BAT_FUSED", "BATT_SW"], note="Series switch")
    g.hdr("J16", "INA260 VIN+/VIN- (jumper if not fitted)", ["BATT_SW", "+BATT"],
          note="INA260 in series with the battery +; link pins 1-2 when absent", flags=("+BATT",))
    g.cap("470uF 25V low-ESR", "+BATT", "GND", ref="C1", polar=True, note="VM bulk, near TB6612")
    g.cap("100nF", "+BATT", "GND", ref="C2")
    g.add("Device", "D_TVS", "D", "TVS >=13V stand-off", FP_TVS, {"1": "+BATT", "2": "GND"},
          note="Clamp VM spikes (TB6612 VM max 13.5 V, check on a scope)")
    g.hdr("J3", "5V buck module (>=4A)", ["+BATT", "GND", "+5V", "GND"],
          note="1 IN+ / 2 IN- / 3 OUT+ / 4 OUT-", flags=("+5V",))
    g.hdr("J4", "3V3 LDO module (sensors)", ["+5V", "GND", "+3V3", "GND"],
          note="1 IN / 2 GND / 3 OUT / 4 GND", flags=("+3V3",))
    g.hdr("J5", "To PB2 5V input (see PB2 manual)", ["+5V", "GND"])
    g.hdr("J6", "To powered USB hub", ["+5V", "GND"])

    # ---------------- column 2: motor driver --------------------------------
    g.column(120, "2. TB6612 DRIVER + STBY GATE")
    g.hdr("J7", "TB6612 module logic side",
          ["+3V3", "GND", "TB_PWMA", "TB_AIN1", "TB_AIN2", "TB_STBY", "TB_BIN1", "TB_BIN2", "TB_PWMB"],
          note="VCC, GND, PWMA, AIN1, AIN2, STBY, BIN1, BIN2, PWMB")
    g.hdr("J8", "TB6612 module power side", ["+BATT", "GND", "ML_P", "ML_N", "MR_P", "MR_N"],
          note="VM, GND, AO1, AO2, BO1, BO2")
    for sig, tb in (("MOT_L_PWM", "TB_PWMA"), ("MOT_L_IN1", "TB_AIN1"), ("MOT_L_IN2", "TB_AIN2"),
                    ("MOT_R_PWM", "TB_PWMB"), ("MOT_R_IN1", "TB_BIN1"), ("MOT_R_IN2", "TB_BIN2")):
        g.res("47R", sig, tb, note="series, edge rate")
        g.res("10k", tb, "GND", note="pull-down: motor off while PB2 boots")
    g.add("74xGxx", "74LVC2G08", "U", "74LVC2G08", FP_U1,
          {"1": "MOT_EN_PRU", "2": "MOT_EN_M4F", "7": "GATE_X"}, unit=1,
          note="STBY = PRU_EN & M4F_EN & ESTOP_OK")
    g.add("74xGxx", "74LVC2G08", "U", "74LVC2G08", FP_U1,
          {"5": "GATE_X", "6": "ESTOP_OK", "3": "TB_STBY"}, unit=2, ref="U1")
    g.add("74xGxx", "74LVC2G08", "U", "74LVC2G08", FP_U1,
          {"GND": "GND", "VCC": "+3V3"}, unit=3, ref="U1")
    g.res("10k", "TB_STBY", "GND", note="STBY pull-down (default: standby)")
    g.hdr("J11", "E-stop (NC contact)", ["+3V3", "ESTOP_OK"],
          note="Closed = run. Open/pressed/wire cut -> pulled low -> motors in standby")
    g.res("100k", "ESTOP_OK", "GND", note="pull-down so a broken wire = stop")
    g.hdr("J12", "ARM button (to GND)", ["ARM_BTN", "GND"])
    g.res("100k", "ARM_BTN", "+3V3", note="pull-up (idle high)")

    # ---------------- column 3: motors + encoders ---------------------------
    g.column(220, "3. GB37-520 MOTORS + ENCODERS")
    for side, s in (("L", "L"), ("R", "R")):
        g.hdr(f"J{9 if side == 'L' else 10}", f"Motor {side} 6-pin (verify your harness order)",
              [f"M{s}_P", "GND", f"ENC_{s}_A_RAW", f"ENC_{s}_B_RAW", "+3V3", f"M{s}_N"],
              note="1 M+ / 2 GND / 3 ENC A / 4 ENC B / 5 ENC VCC (3.3 V) / 6 M-", fp=FP_PH6)
        g.cap("100nF", f"M{s}_P", f"M{s}_N", note="motor EMI suppression")
        for ch in ("A", "B"):
            raw, node, sig = f"ENC_{s}_{ch}_RAW", f"ENC_{s}_{ch}_F", f"ENC_{s}_{ch}"
            g.res("1k", raw, node, note="encoder RC filter + series")
            g.cap("1nF", node, "GND")
            g.res("4.7k DNP", node, "+3V3", note="pull-up only if encoder is open-collector")
            g.res("0R", node, sig, note="link to J_SIG (scope tap point)")

    # ---------------- column 4: sensors/debug -------------------------------
    g.column(320, "4. SENSORS AND DEBUG")
    g.hdr("J14", "MPU-6500 SPI module",
          ["+3V3", "GND", "IMU_SCLK_M", "IMU_MOSI_M", "IMU_MISO_M", "IMU_CS_M", "IMU_INT_M", "GND"],
          note="VCC, GND, SCLK, SDI(MOSI), SDO(MISO), NCS, INT, FSYNC->GND")
    for s in ("SCLK", "MOSI", "MISO", "CS", "INT"):
        g.res("22R", f"IMU_{s}", f"IMU_{s}_M", note="series damping")
    g.hdr("J15", "INA260 / INA226 I2C", ["+3V3", "GND", "I2C_SCL", "I2C_SDA", "INA_ALERT"],
          note="VCC, GND, SCL, SDA, ALERT (I2C bus owner: M4F, see pinmap)")
    g.res("4.7k", "I2C_SCL", "+3V3", note="I2C pull-ups (one set only)")
    g.res("4.7k", "I2C_SDA", "+3V3")
    g.hdr("J17", "UART debug (3.3 V)", ["GND", "UART_TX", "UART_RX"],
          note="TX/RX named from the PB2 side; cross to the adapter")

    # ---------------- column 5: PB2 signal header ---------------------------
    g.column(420, "5. J18 TO PB2 HEADERS (pinmap.yaml)")
    g.hdr("J18", "Logical signals to PB2 headers", SIGNALS + ["GND", "GND"],
          note="Pin order = pinmap.yaml order. Wire each to the header pin chosen in M0.")
    g.text("Rev 0: modules + perfboard. PB2 pin numbers are NOT fixed until milestone M0.", 20, 12, 2.0)
    g.text("Generated by robot/hardware/tools/gen_rev0.py - edit the generator, not this file.", 20, 17, 1.5)

    g.finish(path)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(__file__), "..", "rev0", PROJECT + ".kicad_sch")
    build(out)
    print("wrote", out)
