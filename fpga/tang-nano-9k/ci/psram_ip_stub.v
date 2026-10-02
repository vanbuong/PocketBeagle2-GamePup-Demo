// Black-box stand-in for Gowin's PSRAM_Memory_Interface_HS_Top (the real IP is encrypted and
// generated in Gowin EDA), so CI can synthesise the PSRAM design and check its logic.
(* blackbox *)
module PSRAM_Memory_Interface_HS_Top (
	input clk, memory_clk, pll_lock, rst_n,
	output [1:0] O_psram_ck, O_psram_ck_n,
	inout  [1:0] IO_psram_rwds,
	inout  [15:0] IO_psram_dq,
	output [1:0] O_psram_reset_n, O_psram_cs_n,
	input  [20:0] addr,
	input  [63:0] wr_data,
	output [63:0] rd_data,
	output rd_data_valid,
	input  cmd, cmd_en,
	input  [7:0] data_mask,
	output clk_out,
	output init_calib
);
endmodule
