`timescale 1ns/1ps
// SPI master model + frame-store model; checks the ILI9341-style write path.
module tb_dbi;
	reg clk = 0, rst_n = 0;
	always #18.518 clk = ~clk;            // 27 MHz

	reg sclk = 0, mosi = 0, cs_n = 1, dc = 0;
	wire tog, bdc; wire [7:0] bdat;
	spi_slave spi (.sclk(sclk), .mosi(mosi), .cs_n(cs_n), .dc(dc),
	               .byte_toggle(tog), .byte_data(bdat), .byte_dc(bdc));

	wire wr_en, display_on, sleeping; wire [16:0] wr_addr; wire [15:0] wr_data;
	wire [7:0] madctl, colmod;
	dbi_decoder dut (.clk(clk), .rst_n(rst_n), .byte_toggle(tog), .byte_data(bdat), .byte_dc(bdc),
	                 .wr_en(wr_en), .wr_addr(wr_addr), .wr_data(wr_data),
	                 .display_on(display_on), .sleeping(sleeping), .madctl(madctl), .colmod(colmod));

	reg [15:0] mem [0:130559];
	integer writes = 0, i, errors = 0;
	always @(posedge clk) if (wr_en) begin mem[wr_addr] <= wr_data; writes <= writes + 1; end

	localparam HALF = 10.4;               // ~48 MHz SCLK
	task send_byte(input [7:0] b);
		integer k;
		begin
			for (k = 7; k >= 0; k = k - 1) begin
				mosi = b[k]; #HALF sclk = 1; #HALF sclk = 0;
			end
		end
	endtask
	task cmd(input [7:0] c);
		begin dc = 0; #50 cs_n = 0; #20 send_byte(c); #20 cs_n = 1; #200; end
	endtask
	task data_begin; begin dc = 1; #50 cs_n = 0; #20; end endtask
	task data_end;   begin #20 cs_n = 1; #200; end endtask
	task data1(input [7:0] b); begin data_begin; send_byte(b); data_end; end endtask
	task px(input [15:0] v); begin send_byte(v[15:8]); send_byte(v[7:0]); end endtask
	task window(input [15:0] x0, x1, y0, y1);
		begin
			cmd(8'h2A); data_begin; send_byte(x0[15:8]); send_byte(x0[7:0]); send_byte(x1[15:8]); send_byte(x1[7:0]); data_end;
			cmd(8'h2B); data_begin; send_byte(y0[15:8]); send_byte(y0[7:0]); send_byte(y1[15:8]); send_byte(y1[7:0]); data_end;
		end
	endtask
	task check(input [16:0] a, input [15:0] exp, input [255:0] what);
		begin
			if (mem[a] !== exp) begin
				$display("FAIL %0s: mem[%0d]=%h expected %h", what, a, mem[a], exp);
				errors = errors + 1;
			end
		end
	endtask

	integer x, y;
	initial begin
		for (i = 0; i < 130560; i = i + 1) mem[i] = 16'hXXXX;
		#200 rst_n = 1; #200;

		// init sequence like panel-mipi-dbi
		cmd(8'h01); cmd(8'h11); data1(8'h00);  // stray data with no params: ignored
		cmd(8'h3A); data1(8'h55);
		cmd(8'h36); data1(8'h08);
		cmd(8'h29);
		if (!display_on || sleeping) begin $display("FAIL power flags"); errors = errors + 1; end
		if (colmod !== 8'h55 || madctl !== 8'h08) begin $display("FAIL colmod/madctl"); errors = errors + 1; end

		// 1) small window, 10x3, pixels sent in 3 separate SPI messages (CS toggles)
		window(100, 109, 50, 52);
		cmd(8'h2C);
		for (y = 0; y < 3; y = y + 1) begin
			data_begin;
			for (x = 0; x < 10; x = x + 1) px(16'h1000 * (y + 1) + x);
			data_end;
		end
		for (y = 0; y < 3; y = y + 1)
			for (x = 0; x < 10; x = x + 1)
				check((50 + y) * 480 + 100 + x, 16'h1000 * (y + 1) + x, "small window");
		if (writes !== 30) begin $display("FAIL write count %0d", writes); errors = errors + 1; end

		// 2) window wrap: 4 extra pixels in a 2x2 window overwrite from the start
		window(0, 1, 0, 1);
		cmd(8'h2C);
		data_begin; for (x = 0; x < 6; x = x + 1) px(16'hA000 + x); data_end;
		check(0, 16'hA004, "wrap p0"); check(1, 16'hA005, "wrap p1");
		check(480, 16'hA002, "wrap p2"); check(481, 16'hA003, "wrap p3");

		// 3) full-frame window: last pixel lands at the final address
		window(0, 479, 0, 271);
		cmd(8'h2C);
		data_begin; px(16'h1234); data_end;
		check(0, 16'h1234, "full first");
		// jump near the end via a 1x1 window at (479,271)
		window(479, 479, 271, 271);
		cmd(8'h2C); data_begin; px(16'hBEEF); data_end;
		check(271 * 480 + 479, 16'hBEEF, "last pixel");

		// 4) out-of-range coordinates are dropped
		window(478, 482, 0, 0);
		writes = 0;
		cmd(8'h2C); data_begin; for (x = 0; x < 5; x = x + 1) px(16'hC000 + x); data_end;
		if (writes !== 2) begin $display("FAIL clip writes=%0d", writes); errors = errors + 1; end

		// 5) a partial byte before CS rises is dropped, stream stays aligned
		window(10, 11, 200, 200);
		cmd(8'h2C);
		dc = 1; #50 cs_n = 0; #20 px(16'hD001);
		mosi = 1; #HALF sclk = 1; #HALF sclk = 0; mosi = 1; #HALF sclk = 1; #HALF sclk = 0; // 2 stray bits
		#20 cs_n = 1; #200;
		data_begin; px(16'hD002); data_end;
		check(200 * 480 + 10, 16'hD001, "partial 1"); check(200 * 480 + 11, 16'hD002, "partial 2");

		// 6) reset pin returns to defaults
		rst_n = 0; #100 rst_n = 1; #100;
		if (display_on || !sleeping) begin $display("FAIL reset flags"); errors = errors + 1; end

		if (errors == 0) $display("PASS: dbi decoder");
		else $display("FAIL: %0d errors", errors);
		$finish;
	end
	initial begin #50_000_000 $display("FAIL timeout"); $finish; end
endmodule
