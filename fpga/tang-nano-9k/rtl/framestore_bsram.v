// Frame store, BSRAM version (bring-up).  Same port list as a future PSRAM version:
//   write port  (clk_w): wr_en, wr_addr (y*W+x), wr_data RGB565
//   read  port  (clk_r): rd_x/rd_y (lcd_timing counters), rd_data RGB565, 1 clk latency
// A full 480x272 RGB565 frame (2.09 Mb) does not fit the GW1NR-9's 468 kb of BSRAM,
// so each pixel is reduced to R_BITS/G_BITS/B_BITS (most-significant bits kept) and
// expanded back to RGB565 on read.  Default 1/1/1 = 8 colours: 391,680 bits,
// about 24 of the 26 BSRAM blocks (3 x 8; 16384 x 1 each).
// Budget check (must be <= 468 kb): W*H*(R_BITS+G_BITS+B_BITS)
//   1/1/1 -> 391,680 ok     1/2/1 -> 522,240 too big     (or shrink W/H)
// The two ports may run on different clocks.
module framestore_bsram #(
	parameter W = 480,
	parameter H = 272,
	parameter R_BITS = 1,
	parameter G_BITS = 1,
	parameter B_BITS = 1
) (
	input  wire        clk_w,
	input  wire        wr_en,
	input  wire [16:0] wr_addr,
	input  wire [15:0] wr_data,

	input  wire        clk_r,
	input  wire [9:0]  rd_x,
	input  wire [9:0]  rd_y,
	output reg  [15:0] rd_data
);
	localparam N  = W * H;
	localparam PB = R_BITS + G_BITS + B_BITS;

	wire [PB-1:0] packed_px = {wr_data[15 -: R_BITS], wr_data[10 -: G_BITS], wr_data[4 -: B_BITS]};

	// One 1-bit memory per stored bit, power-of-two depth (2**17 >= W*H), no address range
	// guards: this is the shape Gowin tools map to BSRAM (8 blocks of 16384 x 1 per bit).
	// A single wide array with `addr < N` guards was mapped to ~94,000 LUT/mux cells
	// (yosys synth_gowin) and does not fit the chip.
	// The decoder never writes outside the frame, and reads outside it are blanked.
	wire [PB-1:0] q;
	genvar gb;
	generate for (gb = 0; gb < PB; gb = gb + 1) begin : g_bit
		(* syn_ramstyle = "block_ram", ram_style = "block" *) reg mem [0:131071];
`ifdef SIM
		integer i;
		initial for (i = 0; i < 131072; i = i + 1) mem[i] = 1'b0;   // avoid X in simulation
`endif
		reg qb;
		always @(posedge clk_w) if (wr_en) mem[wr_addr] <= packed_px[gb];
		always @(posedge clk_r) qb <= mem[rd_addr];
		assign q[gb] = qb;
	end endgenerate

	// y*480 = (y<<9) - (y<<5)  (W fixed at 480 here)
	wire [18:0] ya = {rd_y, 9'b0} - {rd_y, 5'b0};
	wire [16:0] rd_addr = ya[16:0] + {7'b0, rd_x};

	// bit-replicate a v-bit value up to n bits
	function [5:0] expand(input [5:0] v, input integer vb, input integer nb);
		integer k;
		reg [5:0] r;
		begin
			r = 0;
			for (k = 0; k < nb; k = k + 1)
				r[nb-1-k] = v[vb-1 - (k % vb)];
			expand = r;
		end
	endfunction

	wire [5:0] r5 = expand(q[PB-1 -: R_BITS],             R_BITS, 5);
	wire [5:0] g6 = expand(q[PB-1-R_BITS -: G_BITS],      G_BITS, 6);
	wire [5:0] b5 = expand(q[B_BITS-1 : 0],               B_BITS, 5);
	always @* rd_data = {r5[4:0], g6[5:0], b5[4:0]};
endmodule
