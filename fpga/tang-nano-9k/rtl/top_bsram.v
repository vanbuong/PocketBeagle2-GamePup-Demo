// Synthesis top for the BSRAM bring-up build (8 colours): no PSRAM pad ports, so the
// tool does not try to place them.  parameter SHOW_PATTERN=1 gives the test pattern.
module top_bsram #(
	parameter SHOW_PATTERN = 0
) (
	input  wire        XTAL_IN,
	input  wire        nRST,
	input  wire        SPI_SCK,
	input  wire        SPI_MOSI,
	input  wire        SPI_CS_N,
	input  wire        DBI_DC,
	input  wire        DBI_RST_N,
	output wire        LCD_CLK,
	output wire        LCD_DEN,
	output wire        LCD_SYNC,
	output wire        LCD_HYNC,
	output wire [4:0]  LCD_R,
	output wire [5:0]  LCD_G,
	output wire [4:0]  LCD_B
);
	top #(.SHOW_PATTERN(SHOW_PATTERN), .FS_PSRAM(0)) u_top (
		.XTAL_IN(XTAL_IN), .nRST(nRST),
		.SPI_SCK(SPI_SCK), .SPI_MOSI(SPI_MOSI), .SPI_CS_N(SPI_CS_N), .DBI_DC(DBI_DC), .DBI_RST_N(DBI_RST_N),
		.LCD_CLK(LCD_CLK), .LCD_DEN(LCD_DEN), .LCD_SYNC(LCD_SYNC), .LCD_HYNC(LCD_HYNC),
		.LCD_R(LCD_R), .LCD_G(LCD_G), .LCD_B(LCD_B),
		.O_psram_ck(), .O_psram_ck_n(), .IO_psram_rwds(), .IO_psram_dq(),
		.O_psram_reset_n(), .O_psram_cs_n()
	);
endmodule
