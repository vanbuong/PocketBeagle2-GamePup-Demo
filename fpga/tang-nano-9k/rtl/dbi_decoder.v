// MIPI-DBI (ILI9341 subset) command decoder -> frame-store write port.
// Runs in the system clock domain. Pixels are RGB565, big-endian on the wire.
//
// Commands: 01 SWRESET, 11 SLPOUT, 10 SLPIN, 29 DISPON, 28 DISPOFF,
//           2A CASET, 2B PASET, 2C RAMWR, 36 MADCTL (stored), 3A COLMOD (stored).
// Everything else is accepted and its parameters ignored.
// The RAMWR pointer survives chip-select toggles (the Linux driver splits long
// pixel streams into several SPI messages).
module dbi_decoder #(
	parameter W = 480,
	parameter H = 272
) (
	input  wire        clk,
	input  wire        rst_n,        // external reset pin, synchronised by caller

	input  wire        byte_toggle,  // from spi_slave (async to clk)
	input  wire [7:0]  byte_data,
	input  wire        byte_dc,

	output reg         wr_en,        // 1 clk pulse per pixel
	output reg  [16:0] wr_addr,      // y*W + x
	output reg  [15:0] wr_data,

	output reg         display_on,
	output reg         sleeping,
	output reg  [7:0]  madctl,
	output reg  [7:0]  colmod
);
	// 2-FF synchroniser on the byte toggle; byte_data is stable by then.
	// No reset on purpose: a reset value taken from byte_toggle is a non-constant async
	// load that Gowin flops cannot implement.  The chain just keeps running; bytes that
	// arrive while rst_n is low are ignored by the decoder state below.
	reg t1 = 1'b0, t2 = 1'b0, t3 = 1'b0;
	always @(posedge clk) begin
		t1 <= byte_toggle; t2 <= t1; t3 <= t2;
	end
	wire new_byte = t2 ^ t3;

	reg [7:0]  cmd;
	reg [2:0]  pcnt;
	reg [15:0] xs, xe, ys, ye, cx, cy;
	reg        hi_valid;
	reg [7:0]  hi;

	wire [15:0] x_last = W - 1;
	wire [15:0] y_last = H - 1;

	task soft_reset;
		begin
			xs <= 0; xe <= x_last; ys <= 0; ye <= y_last; cx <= 0; cy <= 0;
			display_on <= 1'b0; sleeping <= 1'b1;
			madctl <= 8'h00; colmod <= 8'h55;
			cmd <= 8'h00; pcnt <= 0; hi_valid <= 1'b0;
		end
	endtask

	always @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			soft_reset;
			wr_en <= 1'b0; wr_addr <= 0; wr_data <= 0;
		end else begin
			wr_en <= 1'b0;
			if (new_byte) begin
				if (!byte_dc) begin
					// ---- command byte ----
					cmd <= byte_data; pcnt <= 0; hi_valid <= 1'b0;
					case (byte_data)
						8'h01: soft_reset;
						8'h11: sleeping <= 1'b0;
						8'h10: sleeping <= 1'b1;
						8'h29: display_on <= 1'b1;
						8'h28: display_on <= 1'b0;
						8'h2C: begin cx <= xs; cy <= ys; end
						default: ;
					endcase
				end else begin
					// ---- data byte ----
					case (cmd)
						8'h2A, 8'h2B: begin
							pcnt <= pcnt + 3'd1;
							case (pcnt)
								3'd0: if (cmd == 8'h2A) xs[15:8] <= byte_data; else ys[15:8] <= byte_data;
								3'd1: if (cmd == 8'h2A) xs[7:0]  <= byte_data; else ys[7:0]  <= byte_data;
								3'd2: if (cmd == 8'h2A) xe[15:8] <= byte_data; else ye[15:8] <= byte_data;
								3'd3: if (cmd == 8'h2A) xe[7:0]  <= byte_data; else ye[7:0]  <= byte_data;
								default: ;
							endcase
						end
						8'h36: madctl <= byte_data;
						8'h3A: colmod <= byte_data;
						8'h2C: begin
							if (!hi_valid) begin
								hi <= byte_data; hi_valid <= 1'b1;
							end else begin
								hi_valid <= 1'b0;
								if (cx < W && cy < H) begin
									wr_en   <= 1'b1;
									wr_addr <= cy * W + cx;
									wr_data <= {hi, byte_data};
								end
								// advance within the window, wrapping like the real panel
								if (cx >= xe) begin
									cx <= xs;
									cy <= (cy >= ye) ? ys : cy + 16'd1;
								end else begin
									cx <= cx + 16'd1;
								end
							end
						end
						default: ;
					endcase
				end
			end
		end
	end
endmodule
