// 480x272 RGB565 test pattern:
//  - 1 px white border (proves the whole panel area and edge alignment)
//  - 8 colour bars (top 2/3)
//  - grey ramp (bottom 1/3)
//  - centre xhair-hair
module test_pattern #(
	parameter W = 480,
	parameter H = 272
) (
	input  wire [9:0]  x,
	input  wire [9:0]  y,
	output reg  [15:0] rgb565
);
	localparam BAR_W = W / 8; // 60

	wire border = (x == 0) || (x == W - 1) || (y == 0) || (y == H - 1);
	wire xhair  = (x == W / 2) || (y == H / 2);
	// Full-width intermediates, then explicit slices (no implicit truncation warnings).
	wire [31:0] bar_full   = x / BAR_W;
	wire [31:0] ramp5_full = (x * 32) / W;
	wire [31:0] ramp6_full = (x * 64) / W;
	wire [2:0]  bar   = bar_full[2:0];
	wire [4:0]  ramp5 = ramp5_full[4:0];
	wire [5:0]  ramp6 = ramp6_full[5:0];

	reg [15:0] bar_c;
	always @* begin
		case (bar)
			3'd0: bar_c = 16'hFFFF; // white
			3'd1: bar_c = 16'hFFE0; // yellow
			3'd2: bar_c = 16'h07FF; // cyan
			3'd3: bar_c = 16'h07E0; // green
			3'd4: bar_c = 16'hF81F; // magenta
			3'd5: bar_c = 16'hF800; // red
			3'd6: bar_c = 16'h001F; // blue
			default: bar_c = 16'h0000; // black
		endcase
	end

	always @* begin
		if (border || xhair)       rgb565 = 16'hFFFF;
		else if (y < (H * 2) / 3)  rgb565 = bar_c;
		else                       rgb565 = {ramp5, ramp6, ramp5};
	end
endmodule
