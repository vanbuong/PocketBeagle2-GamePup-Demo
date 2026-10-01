// RGB LCD timing generator (DE mode + sync pulses), 480x272 by default.
// Defaults give 525 x 286 total clocks: 9 MHz / (525*286) = 59.94 Hz.
// Porch/sync values are typical 4.3" 480x272 numbers - check the panel datasheet.
module lcd_timing #(
	parameter H_ACTIVE = 480,
	parameter H_FP     = 8,
	parameter H_SYNC   = 4,
	parameter H_BP     = 33,
	parameter V_ACTIVE = 272,
	parameter V_FP     = 2,
	parameter V_SYNC   = 10,
	parameter V_BP     = 2,
	parameter HS_ACTIVE_LOW = 1,
	parameter VS_ACTIVE_LOW = 1
) (
	input  wire        clk,      // pixel clock
	input  wire        rst_n,
	output reg         de,
	output reg         hsync,
	output reg         vsync,
	output reg  [9:0]  x,        // valid while de
	output reg  [9:0]  y,
	output reg         frame_start // 1 clk pulse at x=0,y=0 of active area
);
	localparam H_TOTAL = H_ACTIVE + H_FP + H_SYNC + H_BP;
	localparam V_TOTAL = V_ACTIVE + V_FP + V_SYNC + V_BP;

	reg [10:0] hc;  // 0..H_TOTAL-1, active area first
	reg [9:0]  vc;

	always @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			hc <= 0; vc <= 0;
		end else if (hc == H_TOTAL - 1) begin
			hc <= 0;
			vc <= (vc == V_TOTAL - 1) ? 10'd0 : vc + 1'b1;
		end else begin
			hc <= hc + 1'b1;
		end
	end

	wire h_act = hc < H_ACTIVE;
	wire v_act = vc < V_ACTIVE;
	wire h_syn = hc >= H_ACTIVE + H_FP && hc < H_ACTIVE + H_FP + H_SYNC;
	wire v_syn = vc >= V_ACTIVE + V_FP && vc < V_ACTIVE + V_FP + V_SYNC;

	// Register all outputs together so they stay aligned.
	always @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			de <= 0; hsync <= HS_ACTIVE_LOW; vsync <= VS_ACTIVE_LOW;
			x <= 0; y <= 0; frame_start <= 0;
		end else begin
			de          <= h_act && v_act;
			hsync       <= HS_ACTIVE_LOW ? ~h_syn : h_syn;
			vsync       <= VS_ACTIVE_LOW ? ~v_syn : v_syn;
			x           <= hc[9:0];
			y           <= vc;
			frame_start <= (hc == 0) && (vc == 0);
		end
	end
endmodule
