`timescale 1ns/1ps
module tb_small_lcd;
	reg clk = 0, rst_n = 0, sclk = 0, mosi = 0, cs3_n = 1, dc = 0;
	always #18.518 clk = ~clk;
	wire c, mo, rs, cs, rstn;
	small_lcd_bridge #(.RESET_CYCLES(100)) dut (.clk(clk), .rst_n(rst_n), .sclk(sclk), .mosi(mosi),
		.cs3_n(cs3_n), .dc(dc), .slcd_clk(c), .slcd_mo(mo), .slcd_rs(rs), .slcd_cs(cs), .slcd_rst_n(rstn));
	pullup (rstn);                         // the board's 10k pull-up
	integer errors = 0, i;
	task chk(input a, input b, input [127:0] w); begin
		if (a !== b) begin $display("FAIL %0s: %b vs %b", w, a, b); errors = errors + 1; end end endtask
	initial begin
		#100; chk(rstn, 1'b0, "reset low at start");
		rst_n = 1; #(37.037 * 50); chk(rstn, 1'b0, "reset still low");
		#(37.037 * 80); chk(rstn, 1'b1, "reset released (pulled up)");
		// pass-through of every line, including CS
		for (i = 0; i < 16; i = i + 1) begin
			{sclk, mosi, cs3_n, dc} = i[3:0]; #5;
			chk(c, sclk, "clk"); chk(mo, mosi, "mosi"); chk(cs, cs3_n, "cs"); chk(rs, dc, "dc");
		end
		if (errors == 0) $display("PASS: small LCD bridge"); else $display("FAIL: %0d errors", errors);
		$finish;
	end
endmodule
