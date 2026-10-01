`timescale 1ns/1ps
// End to end: SPI master -> top -> LCD pins.  Writes known pixels over SPI, captures
// one LCD frame from the output pins and compares with the expected 1/1/1 colours.
module tb_top;
	reg xtal = 0, nrst = 0;
	always #18.518 xtal = ~xtal;

	reg sclk = 0, mosi = 0, cs_n = 1, dc = 0, dbi_rst_n = 0;
	wire lcd_clk, den, vsync, hsync; wire [4:0] r; wire [5:0] g; wire [4:0] b;
	top #(.SHOW_PATTERN(0)) dut (
		.XTAL_IN(xtal), .nRST(nrst),
		.SPI_SCK(sclk), .SPI_MOSI(mosi), .SPI_CS_N(cs_n), .DBI_DC(dc), .DBI_RST_N(dbi_rst_n),
		.LCD_CLK(lcd_clk), .LCD_DEN(den), .LCD_SYNC(vsync), .LCD_HYNC(hsync),
		.LCD_R(r), .LCD_G(g), .LCD_B(b));

	localparam HALF = 10.4;
	task send_byte(input [7:0] v); integer k; begin
		for (k = 7; k >= 0; k = k - 1) begin mosi = v[k]; #HALF sclk = 1; #HALF sclk = 0; end end endtask
	task cmd(input [7:0] c); begin dc = 0; #50 cs_n = 0; #20 send_byte(c); #20 cs_n = 1; #200; end endtask
	task data_begin; begin dc = 1; #50 cs_n = 0; #20; end endtask
	task data_end;   begin #20 cs_n = 1; #200; end endtask
	task window(input [15:0] x0, x1, y0, y1); begin
		cmd(8'h2A); data_begin; send_byte(x0[15:8]); send_byte(x0[7:0]); send_byte(x1[15:8]); send_byte(x1[7:0]); data_end;
		cmd(8'h2B); data_begin; send_byte(y0[15:8]); send_byte(y0[7:0]); send_byte(y1[15:8]); send_byte(y1[7:0]); data_end; end endtask

	// colour for a pixel: cycles through 8 colours by x
	function [15:0] color(input integer x);
		reg [2:0] c;
		begin c = x[2:0];
			color = {c[2] ? 5'h1F : 5'h00, c[1] ? 6'h3F : 6'h00, c[0] ? 5'h1F : 5'h00};
		end
	endfunction

	reg [15:0] cap [0:130559];
	integer n = 0, frames_seen = 0, errors = 0, x, y, rows, i;
	reg prev_vs = 1, capturing = 0;
	always @(posedge lcd_clk) begin
		if (capturing) begin
			if (den) begin cap[n] <= {r, g, b}; n = n + 1; end
			if (vsync && !prev_vs) begin   // vsync rising edge = end of pulse -> frame boundary
				frames_seen = frames_seen + 1;
				if (frames_seen == 1) n = 0;
			end
		end
		prev_vs <= vsync;
	end

	initial begin
		#400 nrst = 1; dbi_rst_n = 1; #400;
		cmd(8'h01); cmd(8'h11); cmd(8'h3A); data_begin; send_byte(8'h55); data_end;
		cmd(8'h29);
		// rows 0..2 and 269..271, full width
		window(0, 479, 0, 2);   cmd(8'h2C); data_begin;
		for (i = 0; i < 3 * 480; i = i + 1) begin send_byte(color(i % 480) >> 8); send_byte(color(i % 480)); end
		data_end;
		window(0, 479, 269, 271); cmd(8'h2C); data_begin;
		for (i = 0; i < 3 * 480; i = i + 1) begin send_byte(color(i % 480) >> 8); send_byte(color(i % 480)); end
		data_end;
		// capture exactly one full frame after the next vsync boundary
		capturing = 1;
		wait (frames_seen == 2);
		capturing = 0;
		for (y = 0; y < 272; y = y + 1)
			for (x = 0; x < 480; x = x + 1) begin
				if ((y < 3 || y >= 269) ? (cap[y * 480 + x] !== color(x)) : (cap[y * 480 + x] !== 16'h0000)) begin
					if (errors < 5) $display("FAIL (%0d,%0d): got %h expected %h", x, y, cap[y*480+x],
						(y < 3 || y >= 269) ? color(x) : 16'h0000);
					errors = errors + 1;
				end
			end
		// display off blanks the panel
		cmd(8'h28);
		frames_seen = 0; capturing = 1;
		wait (frames_seen == 2);
		capturing = 0;
		for (i = 0; i < 130560; i = i + 1)
			if (cap[i] !== 16'h0000) begin errors = errors + 1; if (errors < 5) $display("FAIL display-off px %0d = %h", i, cap[i]); end
		if (errors == 0) $display("PASS: SPI -> framestore -> LCD pins");
		else $display("FAIL: %0d pixel errors", errors);
		$finish;
	end
	initial begin #400_000_000 $display("FAIL timeout"); $finish; end
endmodule
