# SPDX-License-Identifier: GPL-2.0-only
# Hardware source checks. KiCad itself is only needed for the export targets.
.PHONY: all gen bom check test pdf netlist
all: gen bom check test

gen:      ; python3 tools/gen_rev0.py rev0/balbot_rev0.kicad_sch
bom:      ; python3 tools/export_bom.py rev0/balbot_rev0.kicad_sch rev0/BOM.csv
check:    ; python3 tools/check_pinmap.py
test:     ; python3 -m pytest -q tests
pdf:      ; kicad-cli sch export pdf --output rev0/balbot_rev0.pdf rev0/balbot_rev0.kicad_sch
netlist:  ; kicad-cli sch export netlist --format kicadsexpr --output rev0/balbot_rev0.net rev0/balbot_rev0.kicad_sch
