// SPI mode-0 slave front end for MIPI-DBI type C.
// Shift register and bit counter run on SCLK; each completed byte is handed to
// the system clock domain with a toggle (byte_toggle) and a stable byte/dc pair.
// D/C is sampled on the first SCLK rising edge of every byte.
// cs_n high asynchronously clears the bit counter (a partial byte is dropped).
// Constraint: the system clock must be >= ~4x SCLK/8 (27 MHz is fine to 50 MHz SCLK).
module spi_slave (
	input  wire       sclk,
	input  wire       mosi,
	input  wire       cs_n,
	input  wire       dc,
	output reg        byte_toggle,   // SCLK domain; flips per completed byte
	output reg  [7:0] byte_data,     // stable until the next 8 SCLKs complete
	output reg        byte_dc
);
	reg [2:0] bitcnt;
	reg [6:0] shift;
	reg       dc_first;

	initial begin byte_toggle = 1'b0; byte_data = 8'h00; byte_dc = 1'b0; end

	always @(posedge sclk or posedge cs_n) begin
		if (cs_n) begin
			bitcnt <= 3'd0;
		end else begin
			shift  <= {shift[5:0], mosi};
			if (bitcnt == 3'd0) dc_first <= dc;
			if (bitcnt == 3'd7) begin
				byte_data   <= {shift, mosi};
				byte_dc     <= dc_first;
				byte_toggle <= ~byte_toggle;
			end
			bitcnt <= bitcnt + 3'd1;
		end
	end
endmodule
