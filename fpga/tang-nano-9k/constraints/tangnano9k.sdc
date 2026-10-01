create_clock -name xtal -period 37.037 -waveform {0 18.518} [get_ports {XTAL_IN}]
create_generated_clock -name pix -source [get_ports {XTAL_IN}] -divide_by 3 [get_pins {u_pll/u_pll/CLKOUT}]
