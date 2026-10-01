create_clock -name xtal -period 37.037 -waveform {0 18.518} [get_ports {XTAL_IN}]
create_generated_clock -name pix -source [get_ports {XTAL_IN}] -divide_by 3 [get_pins {u_pll/u_pll/CLKOUT}]
create_clock -name spi_sclk -period 20.8 [get_ports {SPI_SCK}]
set_clock_groups -asynchronous -group {xtal} -group {pix} -group {spi_sclk}
