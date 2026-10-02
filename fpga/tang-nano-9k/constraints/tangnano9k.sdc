// Clocks that are real ports.  The 9 MHz pixel clock and the 148.5 MHz PSRAM clock come out of
// the rPLLs; Gowin derives those from XTAL_IN, so no create_generated_clock is written here
// (a hand-written one pointed at an instance name that does not exist after synthesis and
// failed with TA2003).  If the timing report shows the pixel clock unconstrained, constrain
// it by the net name shown in the report.
create_clock -name xtal -period 37.037 -waveform {0 18.518} [get_ports {XTAL_IN}]
create_clock -name spi_sclk -period 20.8 -waveform {0 10.4} [get_ports {SPI_SCK}]
set_clock_groups -asynchronous -group {xtal} -group {spi_sclk}
