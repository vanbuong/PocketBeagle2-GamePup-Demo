`timescale 1ns/1ps
// Behavioural stand-in for Gowin's PSRAM_Memory_Interface_HS_Top (burst 16).
// Models only what psram_ctrl relies on, and checks it:
//   * command + write beat 0 in the same cycle, beats 1..3 on the next three cycles
//   * data_mask bit = 1 masks a byte; byte k = bits [8k+7:8k]
//   * 4 consecutive rd_data_valid beats RD_LAT cycles after a read command
//   * minimum spacing between commands (T_CMD) and 16-word burst alignment
// It is written from my reading of Gowin's example, NOT from the encrypted IP:
// passing against it proves the controller is self-consistent, not that the real IP
// behaves the same. Violations print "MODEL ERROR".
module PSRAM_Memory_Interface_HS_Top #(
	parameter RD_LAT = 20,
	parameter T_CMD  = 14,
	parameter CALIB_NS = 20000
) (
	input  wire        clk,
	input  wire        memory_clk,
	input  wire        pll_lock,
	input  wire        rst_n,
	output wire [1:0]  O_psram_ck,
	output wire [1:0]  O_psram_ck_n,
	inout  wire [1:0]  IO_psram_rwds,
	inout  wire [15:0] IO_psram_dq,
	output wire [1:0]  O_psram_reset_n,
	output wire [1:0]  O_psram_cs_n,
	input  wire [20:0] addr,
	input  wire [63:0] wr_data,
	output reg  [63:0] rd_data,
	output reg         rd_data_valid,
	input  wire        cmd,
	input  wire        cmd_en,
	input  wire [7:0]  data_mask,
	output wire        clk_out,
	output reg         init_calib
);
	assign O_psram_ck = 2'b00;  assign O_psram_ck_n = 2'b11;
	assign IO_psram_rwds = 2'bzz;  assign IO_psram_dq = 16'hzzzz;
	assign O_psram_reset_n = 2'b11;  assign O_psram_cs_n = 2'b11;

	reg c = 0;
	always #6.734 c = ~c;               // 74.25 MHz
	assign clk_out = c;

	reg [7:0] mem [0:(1 << 19) - 1];    // 512 KB: enough for 130,560 px
	integer i;
	initial for (i = 0; i < (1 << 19); i = i + 1) mem[i] = 8'hxx;
	initial begin init_calib = 0; rd_data = 0; rd_data_valid = 0; end
	initial #CALIB_NS init_calib = 1;

	integer errors = 0, cycle = 0, last_cmd = -1000, wr_beat = 4, rd_cnt = -1, rd_beat = 0;
	reg [20:0] wa, ra;

	always @(posedge c) begin
		cycle <= cycle + 1;
		rd_data_valid <= 1'b0;

		// write beats (beat 0 is the command cycle itself)
		if (cmd_en && cmd) begin
			wa = addr; wr_beat = 0;
		end
		if (wr_beat < 4) begin
			for (i = 0; i < 8; i = i + 1)
				if (!data_mask[i]) mem[wa * 2 + wr_beat * 8 + i] <= wr_data[8*i +: 8];
			wr_beat = wr_beat + 1;
		end

		if (cmd_en) begin
			if (!init_calib) begin $display("MODEL ERROR: command before init_calib"); errors = errors + 1; end
			if (cycle - last_cmd < T_CMD) begin
				$display("MODEL ERROR: commands %0d clocks apart (< %0d) at %0t", cycle - last_cmd, T_CMD, $time);
				errors = errors + 1;
			end
			if (addr[3:0] != 0) begin $display("MODEL ERROR: unaligned burst address %h", addr); errors = errors + 1; end
			last_cmd = cycle;
			if (!cmd) begin ra = addr; rd_cnt = 0; rd_beat = 0; end
		end

		// read response: RD_LAT clocks after the command, 4 consecutive beats
		if (rd_cnt >= 0) begin
			rd_cnt = rd_cnt + 1;
			if (rd_cnt >= RD_LAT && rd_beat < 4) begin
				for (i = 0; i < 8; i = i + 1) rd_data[8*i +: 8] <= mem[ra * 2 + rd_beat * 8 + i];
				rd_data_valid <= 1'b1;
				rd_beat = rd_beat + 1;
				if (rd_beat == 4) rd_cnt = -1;
			end
		end
	end
endmodule
