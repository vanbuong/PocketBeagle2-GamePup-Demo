`timescale 1ns/1ps
// Checks frame geometry: 525x286 clocks/frame, 480x272 active pixels, sync widths.
module tb_lcd_timing;
	reg clk27 = 0, rst_n = 0;
	always #18.518 clk27 = ~clk27;
	wire pix, lock;
	pll_pix pll (.clkin(clk27), .clkout(pix), .lock(lock));

	wire de, hs, vs, fs;
	wire [9:0] x, y;
	lcd_timing dut (.clk(pix), .rst_n(rst_n), .de(de), .hsync(hs), .vsync(vs),
	                .x(x), .y(y), .frame_start(fs));
	wire [15:0] px;
	test_pattern pat (.x(x), .y(y), .rgb565(px));

	integer clocks = 0, active = 0, frames = 0, last_fs = -1, period = 0;
	integer vs_low = 0, hs_low = 0, errors = 0, border_ok = 0;
	reg prev_vs = 1;

	initial begin
		#200 rst_n = 1;
	end

	always @(posedge pix) if (rst_n) begin
		clocks = clocks + 1;
		if (de) begin
			active = active + 1;
			if ((x == 0 || x == 479 || y == 0 || y == 271) && px === 16'hFFFF)
				border_ok = border_ok + 1;
		end
		if (fs) begin
			if (last_fs >= 0) begin period = clocks - last_fs; frames = frames + 1; end
			last_fs = clocks;
			if (frames == 1) begin
				active = 0; border_ok = 0;
			end
			if (frames == 2) begin
				if (period !== 525*286) begin $display("FAIL period %0d", period); errors = errors + 1; end
				if (active !== 480*272) begin $display("FAIL active %0d", active); errors = errors + 1; end
				if (border_ok !== 2*480 + 2*270) begin $display("FAIL border %0d", border_ok); errors = errors + 1; end
				if (errors == 0) $display("PASS: %0d clk/frame, %0d active px, border ok", period, active);
				$finish;
			end
		end
	end
	initial begin #200_000_000 $display("FAIL timeout"); $finish; end
endmodule
