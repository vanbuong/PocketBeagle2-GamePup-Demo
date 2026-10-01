// Frame store in the on-chip PSRAM: full RGB565, 480x272.  Same ports as
// framestore_bsram, plus the PSRAM pads (passed through to the Gowin IP).
//   write port  (clk_w): wr_en, wr_addr (y*W+x), wr_data
//   read  port  (clk_r): rd_x/rd_y = lcd_timing counters (every clock, blanking
//                        included), rd_data valid 1 clock later for active pixels.
// The read side relies on lcd_timing: x = 0 marks the start of each line (the line
// after the current one is prefetched into a ping-pong line buffer) and y runs
// 0..V_TOTAL-1.
//
// Requires the Gowin "PSRAM Memory Interface HS" IP generated into ip/psram/
// (module PSRAM_Memory_Interface_HS_Top, burst length 16) - see README.
module framestore_psram #(
	parameter W = 480,
	parameter H = 272,
	parameter V_TOTAL = 286,
	parameter ADDR_SHIFT = 0
) (
	input  wire        clk_w,        // 27 MHz crystal (also the IP reference clock)
	input  wire        rst_w_n,      // synchronised reset in clk_w
	input  wire        wr_en,
	input  wire [16:0] wr_addr,
	input  wire [15:0] wr_data,

	input  wire        clk_r,        // pixel clock
	input  wire        rst_r_n,
	input  wire [9:0]  rd_x,
	input  wire [9:0]  rd_y,
	output wire [15:0] rd_data,

	output wire        ready,        // PSRAM calibrated, frame cleared (clk_m domain; static)
	output wire        err_timeout,

	// PSRAM pads
	output wire [1:0]  O_psram_ck,
	output wire [1:0]  O_psram_ck_n,
	inout  wire [1:0]  IO_psram_rwds,
	inout  wire [15:0] IO_psram_dq,
	output wire [1:0]  O_psram_reset_n,
	output wire [1:0]  O_psram_cs_n
);
	// ---------------- clocks / IP ----------------
	wire mem_clk, pll_lock;
	pll_mem u_pll (.clkin(clk_w), .clkout(mem_clk), .lock(pll_lock));

	wire        clk_m;               // IP user clock (74.25 MHz)
	wire        calib;
	wire [20:0] ip_addr;
	wire        ip_cmd, ip_cmd_en;
	wire [63:0] ip_wdata, ip_rdata;
	wire [7:0]  ip_mask;
	wire        ip_rvalid;

	PSRAM_Memory_Interface_HS_Top u_ip (
		.clk(clk_w), .memory_clk(mem_clk), .pll_lock(pll_lock), .rst_n(rst_w_n),
		.O_psram_ck(O_psram_ck), .O_psram_ck_n(O_psram_ck_n), .IO_psram_rwds(IO_psram_rwds),
		.IO_psram_dq(IO_psram_dq), .O_psram_reset_n(O_psram_reset_n), .O_psram_cs_n(O_psram_cs_n),
		.addr(ip_addr), .wr_data(ip_wdata), .rd_data(ip_rdata), .rd_data_valid(ip_rvalid),
		.cmd(ip_cmd), .cmd_en(ip_cmd_en), .data_mask(ip_mask),
		.clk_out(clk_m), .init_calib(calib)
	);

	// reset for the clk_m domain: async assert, sync release
	reg [1:0] m_rst_sr = 2'b00;
	wire m_arst_n = rst_w_n & pll_lock;
	always @(posedge clk_m or negedge m_arst_n)
		if (!m_arst_n) m_rst_sr <= 2'b00; else m_rst_sr <= {m_rst_sr[0], 1'b1};
	wire rst_m_n = m_rst_sr[1];

	// ---------------- write path: clk_w -> clk_m ----------------
	wire        wf_full, wf_empty, wf_pop;
	wire [32:0] wf_q;
	async_fifo #(.WIDTH(33), .AW(6)) u_wfifo (
		.wclk(clk_w), .wrst_n(rst_w_n), .winc(wr_en), .wdata({wr_addr, wr_data}), .wfull(wf_full),
		.rclk(clk_m), .rrst_n(rst_m_n), .rinc(wf_pop), .rdata(wf_q), .rempty(wf_empty)
	);

	// ---------------- line-fetch trigger: clk_r -> clk_m ----------------
	reg [8:0] fl;  reg fv;  reg trig_pend;  reg trig_tog;
	always @(posedge clk_r or negedge rst_r_n) begin
		if (!rst_r_n) begin fl <= 0; fv <= 0; trig_pend <= 0; trig_tog <= 0; end
		else begin
			trig_pend <= (rd_x == 10'd0);
			if (rd_x == 10'd0) begin
				fl <= (rd_y == V_TOTAL - 1) ? 9'd0 : rd_y[8:0] + 9'd1;
				fv <= (rd_y == V_TOTAL - 1) || (rd_y + 10'd1 < H);
			end
			if (trig_pend && fv) trig_tog <= ~trig_tog;
		end
	end
	reg t1, t2, t3;
	always @(posedge clk_m or negedge rst_m_n)
		if (!rst_m_n) begin t1 <= 0; t2 <= 0; t3 <= 0; end
		else begin t1 <= trig_tog; t2 <= t1; t3 <= t2; end
	wire fetch_req = t2 ^ t3;

	// ---------------- controller ----------------
	wire        lb_we;
	wire [7:0]  lb_addr;
	wire [63:0] lb_data;
	psram_ctrl #(.W(W), .H(H), .ADDR_SHIFT(ADDR_SHIFT)) u_ctrl (
		.clk(clk_m), .rst_n(rst_m_n), .calib(calib),
		.fifo_empty(wf_empty), .fifo_q(wf_q), .fifo_pop(wf_pop),
		.fetch_req(fetch_req), .fetch_line(fl),
		.lb_we(lb_we), .lb_addr(lb_addr), .lb_data(lb_data),
		.addr(ip_addr), .cmd(ip_cmd), .cmd_en(ip_cmd_en), .wr_data(ip_wdata), .data_mask(ip_mask),
		.rd_data(ip_rdata), .rd_valid(ip_rvalid),
		.ready(ready), .err_timeout(err_timeout)
	);

	// ---------------- line buffer: 2 x 128 x 64 bit, clk_m write / clk_r read ----------------
	reg [63:0] lbuf [0:255];
	always @(posedge clk_m) if (lb_we) lbuf[lb_addr] <= lb_data;

	reg [63:0] lw;
	reg [1:0]  sel_d;
	always @(posedge clk_r) begin
		lw    <= lbuf[{rd_y[0], rd_x[8:2]}];
		sel_d <= rd_x[1:0];
	end
	assign rd_data = lw[sel_d * 16 +: 16];
endmodule
