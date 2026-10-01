# Simulation (iverilog) and bitstream (Gowin EDA) targets.
.PHONY: sim bitstream bitstream-psram program clean
sim:
	iverilog -g2005 -DSIM -o build_sim.vvp sim/tb_lcd_timing.v rtl/lcd_timing.v rtl/test_pattern.v rtl/pll_pix.v
	vvp build_sim.vvp | grep -E "PASS|FAIL" | tee /dev/stderr | grep -q PASS
	iverilog -g2005 -o build_dbi.vvp sim/tb_dbi.v rtl/spi_slave.v rtl/dbi_decoder.v
	vvp build_dbi.vvp | grep -E "PASS|FAIL" | tee /dev/stderr | grep -q PASS
	iverilog -g2005 -o build_small.vvp sim/tb_small_lcd.v rtl/small_lcd_bridge.v
	vvp build_small.vvp | grep -E "PASS|FAIL" | tee /dev/stderr | grep -q PASS
	iverilog -g2005 -DSIM -o build_top.vvp sim/tb_top.v rtl/*.v sim/psram_ip_model.v
	vvp build_top.vvp | grep -E "PASS|FAIL" | tee /dev/stderr | grep -q PASS
	iverilog -g2005 -DSIM -DPSRAM -o build_psram.vvp sim/tb_top.v rtl/*.v sim/psram_ip_model.v
	vvp build_psram.vvp | grep -E "PASS|FAIL|MODEL ERROR" | tee /dev/stderr | grep -q PASS
bitstream:
	gw_sh build.tcl
bitstream-psram:
	gw_sh build.tcl psram
program: bitstream
	openFPGALoader -b tangnano9k impl/pnr/tangnano9k_lcd.fs
clean:
	rm -rf build_sim.vvp build_dbi.vvp build_small.vvp build_top.vvp build_psram.vvp impl
