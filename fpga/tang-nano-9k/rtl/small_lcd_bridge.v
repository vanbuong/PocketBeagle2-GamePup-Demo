// On-board 1.14" ST7789 (135x240) on the Tang Nano 9K, driven by the PocketBeagle 2
// through the FPGA: SCLK, MOSI and D/C are wires straight through, the panel has its
// own chip select (PB2 P1.04 = SPI2_CS3).  The ST7789 ignores SCL/SDA while CS is high,
// so traffic for the main display (CS0) on the shared SCLK/MOSI/D/C lines does nothing.
// No gating on SCLK on purpose: it keeps clock and data skew to one LUT-free wire.
//
// Reset: the panel's reset pin has a 10k pull-up to 3.3 V and an RC, so it is driven
// open-drain (low, then released).  It is held low for RESET_CYCLES after configuration
// and independent of the PB2's D/C+reset lines (those belong to the main display; the
// Linux driver for this panel has no reset GPIO and sends SWRESET instead).
module small_lcd_bridge #(
	parameter RESET_CYCLES = 2_700_000     // 100 ms at 27 MHz
) (
	input  wire clk,          // 27 MHz
	input  wire rst_n,

	input  wire sclk,
	input  wire mosi,
	input  wire cs3_n,
	input  wire dc,

	output wire slcd_clk,     // ST7789 SCL (PIN76)
	output wire slcd_mo,      // SDA        (PIN77)
	output wire slcd_rs,      // D/C        (PIN49)
	output wire slcd_cs,      // CS         (PIN48)
	output wire slcd_rst_n    // RESET      (PIN47, open-drain)
);
	reg [22:0] cnt = 23'd0;
	reg        done = 1'b0;
	always @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin cnt <= 0; done <= 1'b0; end
		else if (!done) begin
			if (cnt == RESET_CYCLES - 1) done <= 1'b1; else cnt <= cnt + 23'd1;
		end
	end

	assign slcd_clk   = sclk;
	assign slcd_mo    = mosi;
	assign slcd_rs    = dc;
	assign slcd_cs    = cs3_n;
	assign slcd_rst_n = done ? 1'bz : 1'b0;
endmodule
