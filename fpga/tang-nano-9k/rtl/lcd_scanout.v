// Re-aligns the sync signals with the frame store's 1-cycle-latency read data and
// blanks the output.  Output is registered (2 clk total after the timing generator).
module lcd_scanout (
	input  wire        clk,        // pixel clock
	input  wire        de, hs, vs,
	input  wire        blank,      // force black (display off)
	input  wire [15:0] rd_data,    // frame store data for the pixel presented 1 clk ago
	output reg         de_o, hs_o, vs_o,
	output reg  [15:0] px_o
);
	reg de_d, hs_d, vs_d;
	always @(posedge clk) begin
		de_d <= de; hs_d <= hs; vs_d <= vs;
		de_o <= de_d; hs_o <= hs_d; vs_o <= vs_d;
		px_o <= (de_d && !blank) ? rd_data : 16'h0000;
	end
endmodule
