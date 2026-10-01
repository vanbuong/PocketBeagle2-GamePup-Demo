// Tang Nano 9K - SPI-slave 480x272 RGB LCD display.
//   SHOW_PATTERN=1 : milestone-1 test pattern (no SPI needed)
//   SHOW_PATTERN=0 : SPI (MIPI-DBI) -> frame store (BSRAM, reduced colour) -> LCD
module top #(
	parameter SHOW_PATTERN = 0
) (
	input  wire        XTAL_IN,     // 27 MHz
	input  wire        nRST,        // user button S1 (active low)

	// SPI slave (Linux panel-mipi-dbi master)
	input  wire        SPI_SCK,
	input  wire        SPI_MOSI,
	input  wire        SPI_CS_N,
	input  wire        DBI_DC,
	input  wire        DBI_RST_N,

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

	// ---- resets: async assert, synchronous release ----
	reg [1:0] sys_rst_sr = 2'b00, pix_rst_sr = 2'b00, dbi_rst_sr = 2'b00;
	wire sys_arst_n = nRST;
	always @(posedge XTAL_IN or negedge sys_arst_n)
		if (!sys_arst_n) sys_rst_sr <= 2'b00; else sys_rst_sr <= {sys_rst_sr[0], 1'b1};
	wire pix_arst_n = nRST & pll_lock;
	always @(posedge pix_clk or negedge pix_arst_n)
		if (!pix_arst_n) pix_rst_sr <= 2'b00; else pix_rst_sr <= {pix_rst_sr[0], 1'b1};
	wire dbi_arst_n = nRST & DBI_RST_N;   // DBI reset affects decoder only
	always @(posedge XTAL_IN or negedge dbi_arst_n)
		if (!dbi_arst_n) dbi_rst_sr <= 2'b00; else dbi_rst_sr <= {dbi_rst_sr[0], 1'b1};
	wire pix_rst_n = pix_rst_sr[1];
	wire dbi_rst_n = dbi_rst_sr[1];

	// ---- LCD timing ----
	wire        t_de, t_hs, t_vs;
	wire [9:0]  t_x, t_y;
	wire        frame_start;
	lcd_timing u_timing (
		.clk(pix_clk), .rst_n(pix_rst_n),
		.de(t_de), .hsync(t_hs), .vsync(t_vs), .x(t_x), .y(t_y),
		.frame_start(frame_start)
	);

	wire        o_de, o_hs, o_vs;
	wire [15:0] o_px;

	generate if (SHOW_PATTERN) begin : g_pat
		wire [15:0] px;
		test_pattern u_pat (.x(t_x), .y(t_y), .rgb565(px));
		assign o_de = t_de; assign o_hs = t_hs; assign o_vs = t_vs;
		assign o_px = t_de ? px : 16'h0000;
	end else begin : g_fb
		// ---- SPI -> decoder ----
		wire        b_tog, b_dc;
		wire [7:0]  b_dat;
		spi_slave u_spi (.sclk(SPI_SCK), .mosi(SPI_MOSI), .cs_n(SPI_CS_N), .dc(DBI_DC),
		                 .byte_toggle(b_tog), .byte_data(b_dat), .byte_dc(b_dc));

		wire        wr_en, display_on, sleeping;
		wire [16:0] wr_addr;
		wire [15:0] wr_data;
		wire [7:0]  madctl, colmod;
		dbi_decoder u_dbi (
			.clk(XTAL_IN), .rst_n(dbi_rst_n),
			.byte_toggle(b_tog), .byte_data(b_dat), .byte_dc(b_dc),
			.wr_en(wr_en), .wr_addr(wr_addr), .wr_data(wr_data),
			.display_on(display_on), .sleeping(sleeping), .madctl(madctl), .colmod(colmod)
		);

		// display_on crosses into the pixel domain (static level; 2-FF sync)
		reg [1:0] on_sr = 2'b00;
		always @(posedge pix_clk) on_sr <= {on_sr[0], display_on};

		// ---- frame store ----
		wire [16:0] rd_addr;
		wire [15:0] rd_data;
		framestore_bsram u_fs (
			.clk_w(XTAL_IN), .wr_en(wr_en), .wr_addr(wr_addr), .wr_data(wr_data),
			.clk_r(pix_clk), .rd_addr(rd_addr), .rd_data(rd_data)
		);

		lcd_scanout u_scan (
			.clk(pix_clk), .de(t_de), .hs(t_hs), .vs(t_vs), .x(t_x), .y(t_y),
			.blank(~on_sr[1]), .rd_addr(rd_addr), .rd_data(rd_data),
			.de_o(o_de), .hs_o(o_hs), .vs_o(o_vs), .px_o(o_px)
		);
	end endgenerate

	assign LCD_CLK  = pix_clk;
	assign LCD_DEN  = o_de;
	assign LCD_HYNC = o_hs;
	assign LCD_SYNC = o_vs;
	assign LCD_R = o_px[15:11];
	assign LCD_G = o_px[10:5];
	assign LCD_B = o_px[4:0];
endmodule
