// 27 MHz crystal -> 9 MHz pixel clock.
// Gowin rPLL: VCO = 27 * (15+1) / (0+1) = 432 MHz, / ODIV 48 = 9 MHz.
// NOT yet verified in Gowin EDA - confirm with the IP generator.
// Define SIM for a behavioural stand-in (used by the testbench).
module pll_pix (
	input  wire clkin,   // 27 MHz
	output wire clkout,  // 9 MHz
	output wire lock
);
`ifdef SIM
	reg [1:0] cnt = 0;
	reg       q   = 0;
	always @(posedge clkin) begin
		cnt <= (cnt == 2) ? 2'd0 : cnt + 1'b1;
		if (cnt == 0) q <= 1'b1; else if (cnt == 2 /* ~1/3 duty ok */) q <= 1'b0;
	end
	assign clkout = q;
	assign lock = 1'b1;
`else
	wire clkoutp, clkoutd, clkoutd3;
	rPLL #(
		.FCLKIN("27"),
		.IDIV_SEL(0),
		.FBDIV_SEL(15),
		.ODIV_SEL(48),
		.DEVICE("GW1NR-9C")
	) u_pll (
		.CLKIN(clkin), .CLKFB(1'b0), .RESET(1'b0), .RESET_P(1'b0),
		.FBDSEL(6'b0), .IDSEL(6'b0), .ODSEL(6'b0),
		.PSDA(4'b0), .DUTYDA(4'b0), .FDLY(4'b0),
		.CLKOUT(clkout), .LOCK(lock),
		.CLKOUTP(clkoutp), .CLKOUTD(clkoutd), .CLKOUTD3(clkoutd3)
	);
`endif
endmodule
