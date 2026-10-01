// Frame-store engine for the Tang Nano 9K PSRAM (Gowin "PSRAM Memory Interface HS",
// burst length 16, 64-bit user bus = 4 beats = 16 RGB565 pixels = 32 bytes per burst).
// Runs entirely in the IP's clk_out domain.
//
//  * Writes: pixels (address, RGB565) are popped from a CDC FIFO, merged into one
//    16-pixel burst (byte-masked), and flushed on a burst change, a full burst or idle.
//  * Reads: a "fetch line N" request makes it read the 30 bursts of line N into a
//    line buffer (64-bit words) that the pixel clock domain scans out.
//  * After calibration the whole frame is cleared to black.
//
// Interface assumptions (from Gowin's example for this IP; verify against IPUG943):
//   cmd 1=write 0=read; command + write beat 0 in the same cycle, beats 1..3 on the
//   following 3 cycles; data_mask bit=1 masks a byte; byte k = bits[8k+7:8k];
//   read data arrives as 4 consecutive rd_data_valid beats; addr is a 16-bit-word
//   address (pixel index << ADDR_SHIFT); minimum command spacing T_CMD (=14) clocks.
module psram_ctrl #(
	parameter W          = 480,
	parameter H          = 272,
	parameter ADDR_SHIFT = 0,      // 0: IP address unit is a 16-bit word (= one pixel)
	parameter T_CMD      = 14,
	parameter FLUSH_IDLE = 24      // clocks without new pixels before a partial burst is written
) (
	input  wire        clk,
	input  wire        rst_n,
	input  wire        calib,

	// pixel write FIFO (FWFT): {addr[16:0], data[15:0]}
	input  wire        fifo_empty,
	input  wire [32:0] fifo_q,
	output wire        fifo_pop,

	// line fetch request (clk domain, one pulse)
	input  wire        fetch_req,
	input  wire [8:0]  fetch_line,

	// line buffer write port: 2 lines x 128 words x 64 bit
	output reg         lb_we,
	output reg  [7:0]  lb_addr,
	output reg  [63:0] lb_data,

	// IP user interface
	output reg  [20:0] addr,
	output reg         cmd,
	output reg         cmd_en,
	output reg  [63:0] wr_data,
	output reg  [7:0]  data_mask,
	input  wire [63:0] rd_data,
	input  wire        rd_valid,

	output wire        ready,      // calibrated and frame cleared
	output reg         err_timeout
);
	localparam NBURST = (W * H) / 16;         // 8160
	localparam BPL    = W / 16;               // bursts per line (30)

	localparam S_IDLE = 2'd0, S_WR = 2'd1, S_RD = 2'd2;
	reg [1:0] st;
	reg [6:0] cnt;
	reg       clear_done;
	reg [12:0] clr;
	reg       last_rd;
	assign ready = clear_done;

	// ---- write combiner ----
	reg         cb_valid;
	reg  [12:0] cb_base;
	reg  [255:0] cb_pix;
	reg  [15:0] cb_pres;
	reg  [7:0]  cb_idle;

	wire [16:0] f_addr = fifo_q[32:16];
	wire [15:0] f_data = fifo_q[15:0];
	wire [12:0] f_base = f_addr[16:4];
	wire [3:0]  f_off  = f_addr[3:0];
	wire        same   = cb_valid && (f_base == cb_base);

	wire flush_needed = cb_valid &&
		((!fifo_empty && !same) || (cb_pres == 16'hFFFF) || (cb_idle >= FLUSH_IDLE));
	wire sel_clear = !clear_done;

	// ---- line fetch ----
	reg        fetch_pend, fetch_act;
	reg  [8:0] fetch_pend_line, f_line;
	reg  [4:0] f_k;
	reg  [16:0] rd_px;
	reg  [2:0] rbeat;

	wire want_rd = fetch_act;
	wire want_wr = sel_clear || flush_needed;
	wire act_fetch = (st == S_IDLE) && fetch_pend && !fetch_act && clear_done;
	wire choose_rd = (st == S_IDLE) && !act_fetch && want_rd && clear_done && (!want_wr || !last_rd);
	wire choose_wr = (st == S_IDLE) && !act_fetch && !choose_rd && want_wr;

	assign fifo_pop = clear_done && !fifo_empty && (!cb_valid || same) && !choose_wr;

	function [7:0] mask_beat(input [15:0] pres, input [1:0] j);
		reg [3:0] p;
		begin
			p = pres >> (j * 4);
			mask_beat = {~p[3], ~p[3], ~p[2], ~p[2], ~p[1], ~p[1], ~p[0], ~p[0]};
		end
	endfunction

	wire [255:0] w_pix  = sel_clear ? 256'b0   : cb_pix;
	wire [15:0]  w_pres = sel_clear ? 16'hFFFF : cb_pres;
	wire [12:0]  w_base = sel_clear ? clr      : cb_base;

	wire [6:0] widx = {f_k, 2'b00} + {5'b0, rbeat[1:0]};   // word within the line (0..119)

	reg [255:0] snap_pix;
	reg [15:0]  snap_pres;

	always @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			st <= S_IDLE; cnt <= 0; clear_done <= 1'b0; clr <= 0; last_rd <= 1'b0;
			cb_valid <= 1'b0; cb_base <= 0; cb_pix <= 0; cb_pres <= 0; cb_idle <= 0;
			fetch_pend <= 1'b0; fetch_act <= 1'b0; fetch_pend_line <= 0; f_line <= 0; f_k <= 0;
			rd_px <= 0; rbeat <= 0;
			lb_we <= 1'b0; lb_addr <= 0; lb_data <= 0;
			addr <= 0; cmd <= 1'b0; cmd_en <= 1'b0; wr_data <= 0; data_mask <= 8'hFF;
			snap_pix <= 0; snap_pres <= 0; err_timeout <= 1'b0;
		end else begin
			cmd_en <= 1'b0;
			lb_we  <= 1'b0;

			// requests are latched whatever the state
			if (fetch_req) begin fetch_pend <= 1'b1; fetch_pend_line <= fetch_line; end

			// write combiner: accept a pixel
			if (fifo_pop) begin
				cb_valid <= 1'b1;
				if (!cb_valid) begin cb_base <= f_base; cb_pres <= 16'b0; end
				cb_pix[f_off * 16 +: 16] <= f_data;
				cb_pres[f_off] <= 1'b1;
				cb_idle <= 0;
			end else if (cb_valid && cb_idle != 8'hFF) begin
				cb_idle <= cb_idle + 8'd1;
			end

			case (st)
			S_IDLE: begin
				if (!calib) begin
					// wait for PSRAM calibration
				end else if (act_fetch) begin
					fetch_act <= 1'b1; fetch_pend <= fetch_req;   // keep a request that arrives now
					f_line <= fetch_pend_line; f_k <= 0;
					rd_px <= {fetch_pend_line, 9'b0} - {fetch_pend_line, 5'b0};  // line * 480
				end else if (choose_rd) begin
					addr <= rd_px << ADDR_SHIFT; cmd <= 1'b0; cmd_en <= 1'b1; data_mask <= 8'h00;
					rd_px <= rd_px + 17'd16;
					cnt <= 0; rbeat <= 0; st <= S_RD; last_rd <= 1'b1;
				end else if (choose_wr) begin
					addr <= {w_base, 4'b0} << ADDR_SHIFT; cmd <= 1'b1; cmd_en <= 1'b1;
					wr_data <= w_pix[63:0]; data_mask <= mask_beat(w_pres, 2'd0);
					snap_pix <= w_pix; snap_pres <= w_pres;
					if (sel_clear) begin
						clr <= clr + 13'd1;
						if (clr == NBURST - 1) clear_done <= 1'b1;
					end else begin
						cb_valid <= 1'b0; cb_pres <= 16'b0;
					end
					cnt <= 0; st <= S_WR; last_rd <= 1'b0;
				end
			end

			S_WR: begin
				cnt <= cnt + 7'd1;
				case (cnt)
					7'd0: begin wr_data <= snap_pix[127:64];  data_mask <= mask_beat(snap_pres, 2'd1); end
					7'd1: begin wr_data <= snap_pix[191:128]; data_mask <= mask_beat(snap_pres, 2'd2); end
					7'd2: begin wr_data <= snap_pix[255:192]; data_mask <= mask_beat(snap_pres, 2'd3); end
					default: data_mask <= 8'hFF;
				endcase
				if (cnt == T_CMD - 2) st <= S_IDLE;
			end

			S_RD: begin
				cnt <= cnt + 7'd1;
				if (rd_valid && rbeat < 3'd4) begin
					lb_we   <= 1'b1;
					lb_addr <= {f_line[0], widx};
					lb_data <= rd_data;
					rbeat   <= rbeat + 3'd1;
				end
				if (cnt >= T_CMD - 2 && (rbeat == 3'd4 || (rbeat == 3'd3 && rd_valid))) begin
					st <= S_IDLE;
					if (f_k == BPL - 1) fetch_act <= 1'b0; else f_k <= f_k + 5'd1;
				end else if (cnt == 7'd120) begin
					err_timeout <= 1'b1;            // IP never returned the data: give up this line
					st <= S_IDLE; fetch_act <= 1'b0;
				end
			end
			default: st <= S_IDLE;
			endcase
		end
	end
endmodule
