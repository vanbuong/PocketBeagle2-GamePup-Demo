# Simulation (iverilog) and bitstream (Gowin EDA) targets.
.PHONY: sim bitstream program clean
sim:
	iverilog -g2005 -DSIM -o build_sim.vvp sim/tb_lcd_timing.v rtl/lcd_timing.v rtl/test_pattern.v rtl/pll_pix.v
	vvp build_sim.vvp | grep -E "PASS|FAIL" | tee /dev/stderr | grep -q PASS
bitstream:
	gw_sh build.tcl
program: bitstream
	openFPGALoader -b tangnano9k impl/pnr/tangnano9k_lcd.fs
clean:
	rm -rf build_sim.vvp impl
