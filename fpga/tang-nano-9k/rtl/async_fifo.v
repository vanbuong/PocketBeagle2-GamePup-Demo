// Dual-clock FIFO, gray-coded pointers, first-word-fall-through read (rdata is
// valid whenever rempty is low).  Writes while full are dropped.
module async_fifo #(
	parameter WIDTH = 33,
	parameter AW    = 6            // depth = 2**AW
) (
	input  wire             wclk,
	input  wire             wrst_n,
	input  wire             winc,
	input  wire [WIDTH-1:0] wdata,
	output wire             wfull,

	input  wire             rclk,
	input  wire             rrst_n,
	input  wire             rinc,
	output wire [WIDTH-1:0] rdata,
	output wire             rempty
);
	reg [WIDTH-1:0] mem [0:(1 << AW) - 1];
	reg [AW:0] wbin, wgray, rbin, rgray;
	reg [AW:0] rgray_w1, rgray_w2, wgray_r1, wgray_r2;

	wire wen = winc & ~wfull;
	wire ren = rinc & ~rempty;
	wire [AW:0] wbin_n = wbin + {{AW{1'b0}}, wen};
	wire [AW:0] rbin_n = rbin + {{AW{1'b0}}, ren};

	always @(posedge wclk or negedge wrst_n)
		if (!wrst_n) begin wbin <= 0; wgray <= 0; end
		else begin wbin <= wbin_n; wgray <= (wbin_n >> 1) ^ wbin_n; end
	always @(posedge wclk) if (wen) mem[wbin[AW-1:0]] <= wdata;

	always @(posedge rclk or negedge rrst_n)
		if (!rrst_n) begin rbin <= 0; rgray <= 0; end
		else begin rbin <= rbin_n; rgray <= (rbin_n >> 1) ^ rbin_n; end

	always @(posedge wclk or negedge wrst_n)
		if (!wrst_n) begin rgray_w1 <= 0; rgray_w2 <= 0; end
		else begin rgray_w1 <= rgray; rgray_w2 <= rgray_w1; end
	always @(posedge rclk or negedge rrst_n)
		if (!rrst_n) begin wgray_r1 <= 0; wgray_r2 <= 0; end
		else begin wgray_r1 <= wgray; wgray_r2 <= wgray_r1; end

	assign wfull  = (wgray == {~rgray_w2[AW:AW-1], rgray_w2[AW-2:0]});
	assign rempty = (rgray == wgray_r2);
	assign rdata  = mem[rbin[AW-1:0]];
endmodule
