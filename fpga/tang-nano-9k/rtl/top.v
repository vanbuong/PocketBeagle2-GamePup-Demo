// Tang Nano 9K - 480x272 RGB LCD test pattern (milestone 1).
module top (
	input  wire        XTAL_IN,     // 27 MHz
	input  wire        nRST,        // user button S1 (active low)
	output wire        LCD_CLK,
	output wire        LCD_DEN,
	output wire        LCD_SYNC,    // VSYNC
	output wire        LCD_HYNC,    // HSYNC
	output wire [4:0]  LCD_R,
	output wire [5:0]  LCD_G,
	output wire [4:0]  LCD_B
);
	wire pix_clk, pll_lock;
	pll_pix u_pll (.clkin(XTAL_IN), .clkout(pix_clk), .lock(pll_lock));

	wire rst_n = nRST & pll_lock;

	wire        de, hs, vs;
	wire [9:0]  x, y;
	wire        frame_start;
	lcd_timing u_timing (
		.clk(pix_clk), .rst_n(rst_n),
		.de(de), .hsync(hs), .vsync(vs), .x(x), .y(y),
		.frame_start(frame_start)
	);

	wire [15:0] px;
	test_pattern u_pat (.x(x), .y(y), .rgb565(px));

	assign LCD_CLK  = pix_clk;
	assign LCD_DEN  = de;
	assign LCD_HYNC = hs;
	assign LCD_SYNC = vs;
	assign LCD_R = de ? px[15:11] : 5'd0;
	assign LCD_G = de ? px[10:5]  : 6'd0;
	assign LCD_B = de ? px[4:0]   : 5'd0;
endmodule
