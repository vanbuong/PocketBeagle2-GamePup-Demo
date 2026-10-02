#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Open-source Gowin flow (yosys + nextpnr-himbaechel + apycula) for the Tang Nano 9K.
#
#   ci/build-oss.sh bsram        synthesise, place&route, check timing, pack tangnano9k_lcd.fs
#   ci/build-oss.sh psram-synth  synthesise the PSRAM design with the Gowin IP as a black box
#                                and check it fits (it cannot be placed without the IP)
#   ci/build-oss.sh pll          pack a tiny design with both PLLs to validate their parameters
#   ci/build-oss.sh all          all of the above
#
# Tools: pip install -r ci/requirements.txt  (YoWASP tools only see paths under the working
# directory, so everything is relative to fpga/tang-nano-9k and written to build/oss/.)
# FREQ (default 50) is the clock target nextpnr checks every clock against; SEED the P&R seed.
set -eu

cd "$(dirname "$0")/.."
OUT=build/oss
FREQ=${FREQ:-50}
SEED=${SEED:-1}
DEVICE=GW1NR-LV9QN88PC6/I5
FAMILY=GW1N-9C
LUT_LIMIT=8640

for tool in yowasp-yosys yowasp-nextpnr-himbaechel-gowin gowin_pack; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "$tool not found: pip install -r ci/requirements.txt" >&2
		exit 1
	}
done
mkdir -p "$OUT"

COMMON_RTL="rtl/lcd_timing.v rtl/test_pattern.v rtl/pll_pix.v rtl/spi_slave.v rtl/dbi_decoder.v \
rtl/framestore_bsram.v rtl/lcd_scanout.v rtl/small_lcd_bridge.v rtl/top.v"
PSRAM_RTL="rtl/async_fifo.v rtl/psram_ctrl.v rtl/pll_mem.v rtl/framestore_psram.v rtl/top_psram.v"

summary() {
	echo "$*"
	[ -z "${GITHUB_STEP_SUMMARY:-}" ] || echo "$*" >>"$GITHUB_STEP_SUMMARY"
}

# --- yosys error gate ---
check_yosys_log() {
	if grep -E '^ERROR|^Error:' "$1" >/dev/null; then
		grep -E '^ERROR|^Error:' "$1" >&2
		exit 1
	fi
}

bsram() {
	yowasp-yosys -q -l "$OUT/bsram-yosys.log" -p \
		"read_verilog $COMMON_RTL rtl/top_bsram.v; synth_gowin -top top_bsram -json $OUT/bsram-synth.json"
	check_yosys_log "$OUT/bsram-yosys.log"
	yowasp-nextpnr-himbaechel-gowin --json "$OUT/bsram-synth.json" --write "$OUT/bsram-pnr.json" \
		--device "$DEVICE" --vopt family=$FAMILY --vopt cst=constraints/tangnano9k_lcd.cst \
		--freq "$FREQ" --seed "$SEED" >"$OUT/bsram-nextpnr.log" 2>&1 || {
		tail -30 "$OUT/bsram-nextpnr.log" >&2
		exit 1
	}
	if grep -q '(FAIL at' "$OUT/bsram-nextpnr.log"; then
		grep 'Max frequency' "$OUT/bsram-nextpnr.log" >&2
		echo "timing not met at $FREQ MHz" >&2
		exit 1
	fi
	gowin_pack -d $FAMILY -o "$OUT/tangnano9k_lcd.fs" "$OUT/bsram-pnr.json"
	summary "### Tang Nano 9K BSRAM build (open-source flow, target $FREQ MHz)"
	summary '```'
	summary "$(sed -n '/Device utilisation/,/^Info: Placed/p' "$OUT/bsram-nextpnr.log" | grep -E 'LUT4|DFF|BSRAM|DSP|rPLL|IOB')"
	summary "$(grep 'Max frequency' "$OUT/bsram-nextpnr.log" | tail -4)"
	summary '```'
	ls -l "$OUT/tangnano9k_lcd.fs"
}

psram_synth() {
	yowasp-yosys -q -l "$OUT/psram-yosys.log" -p \
		"read_verilog $COMMON_RTL $PSRAM_RTL ci/psram_ip_stub.v; synth_gowin -top top_psram -json $OUT/psram-synth.json; tee -q -o $OUT/psram-stat.json stat -json"
	check_yosys_log "$OUT/psram-yosys.log"
	python3 - "$OUT/psram-stat.json" "$LUT_LIMIT" <<'PY'
import json, sys
stat = json.load(open(sys.argv[1]))
limit = int(sys.argv[2])
cells = stat["design"]["num_cells_by_type"] if "design" in stat else {}
luts = sum(v for k, v in cells.items() if k.startswith("LUT"))
ffs = sum(v for k, v in cells.items() if k.startswith("DFF"))
bsram = sum(v for k, v in cells.items() if k in ("DP", "DPB", "SDP", "SDPB", "SP", "SPX9", "DPX9B", "SDPX9B"))
print(f"PSRAM build (yosys estimate): {luts} LUTs of {limit}, {ffs} flip-flops, {bsram} block RAMs")
if "GITHUB_STEP_SUMMARY" in __import__("os").environ:
    with open(__import__("os").environ["GITHUB_STEP_SUMMARY"], "a") as f:
        f.write(f"### PSRAM build, synthesis only (Gowin IP black-boxed)\n{luts} LUTs of {limit}, {ffs} flip-flops, {bsram} block RAMs\n")
if luts > limit:
    sys.exit(f"PSRAM design needs {luts} LUTs, the chip has {limit}")
PY
}

pll() {
	yowasp-yosys -q -l "$OUT/pll-yosys.log" -p \
		"read_verilog rtl/pll_pix.v rtl/pll_mem.v ci/pll_check.v; synth_gowin -top pll_check -json $OUT/pll-synth.json"
	check_yosys_log "$OUT/pll-yosys.log"
	yowasp-nextpnr-himbaechel-gowin --json "$OUT/pll-synth.json" --write "$OUT/pll-pnr.json" \
		--device "$DEVICE" --vopt family=$FAMILY --vopt cst=ci/pll_check.cst --freq 27 \
		>"$OUT/pll-nextpnr.log" 2>&1 || { tail -20 "$OUT/pll-nextpnr.log" >&2; exit 1; }
	gowin_pack -d $FAMILY -o "$OUT/pll_check.fs" "$OUT/pll-pnr.json"
	echo "PLL parameters accepted by gowin_pack (VCO/PFD ranges)"
}

case "${1:-all}" in
bsram) bsram ;;
psram-synth) psram_synth ;;
pll) pll ;;
all) pll; bsram; psram_synth ;;
*) echo "usage: $0 bsram|psram-synth|pll|all" >&2; exit 2 ;;
esac
