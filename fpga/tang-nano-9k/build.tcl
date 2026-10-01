# Gowin EDA command line build:  gw_sh build.tcl
set_device GW1NR-LV9QN88PC6/I5 -name GW1NR-9C
add_file rtl/lcd_timing.v
add_file rtl/test_pattern.v
add_file rtl/pll_pix.v
add_file rtl/top.v
add_file constraints/tangnano9k_lcd.cst
add_file constraints/tangnano9k.sdc
set_option -top_module top
set_option -verilog_std v2001
set_option -use_sspi_as_gpio 1
set_option -use_mspi_as_gpio 1
set_option -use_done_as_gpio 1
set_option -use_ready_as_gpio 1
set_option -use_cpu_as_gpio 1
set_option -output_base_name tangnano9k_lcd
run all
