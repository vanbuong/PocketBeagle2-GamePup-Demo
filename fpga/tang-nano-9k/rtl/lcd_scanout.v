// Turns lcd_timing's x/y into a frame-store read address and re-aligns the sync
// signals with the 1-cycle-latency read data.  Output is registered (2 clk total).
module lcd_scanout #(
	parameter W = 480
) (
	input  wire        clk,        // pixel clock
	input  wire        de, hs, vs,
	input  wire [9:0]  x, y,
	input  wire        blank,      // force black (display off)
	output wire [16:0] rd_addr,
	input  wire [15:0] rd_data,
	output reg         de_o, hs_o, vs_o,
	output reg  [15:0] px_o
);
	// y*480 = (y<<9) - (y<<5)  (W fixed at 480 here)
	wire [18:0] ya = {y, 9'b0} - {y, 5'b0};
	assign rd_addr = ya[16:0] + {7'b0, x};

	reg de_d, hs_d, vs_d;
	always @(posedge clk) begin
		de_d <= de; hs_d <= hs; vs_d <= vs;
		de_o <= de_d; hs_o <= hs_d; vs_o <= vs_d;
		px_o <= (de_d && !blank) ? rd_data : 16'h0000;
	end
endmodule
