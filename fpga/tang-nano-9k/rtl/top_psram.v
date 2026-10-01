// Synthesis top for the PSRAM build (full RGB565).  The PSRAM pad ports must exist on
// the top level for the Gowin IP; they are internal pins and need no LOC constraints.
module top_psram (
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
	output wire [4:0]  LCD_B,
	output wire [1:0]  O_psram_ck,
	output wire [1:0]  O_psram_ck_n,
	inout  wire [1:0]  IO_psram_rwds,
	inout  wire [15:0] IO_psram_dq,
	output wire [1:0]  O_psram_reset_n,
	output wire [1:0]  O_psram_cs_n
);
	top #(.SHOW_PATTERN(0), .FS_PSRAM(1)) u_top (
		.XTAL_IN(XTAL_IN), .nRST(nRST),
		.SPI_SCK(SPI_SCK), .SPI_MOSI(SPI_MOSI), .SPI_CS_N(SPI_CS_N), .DBI_DC(DBI_DC), .DBI_RST_N(DBI_RST_N),
		.LCD_CLK(LCD_CLK), .LCD_DEN(LCD_DEN), .LCD_SYNC(LCD_SYNC), .LCD_HYNC(LCD_HYNC),
		.LCD_R(LCD_R), .LCD_G(LCD_G), .LCD_B(LCD_B),
		.O_psram_ck(O_psram_ck), .O_psram_ck_n(O_psram_ck_n), .IO_psram_rwds(IO_psram_rwds),
		.IO_psram_dq(IO_psram_dq), .O_psram_reset_n(O_psram_reset_n), .O_psram_cs_n(O_psram_cs_n)
	);
endmodule
