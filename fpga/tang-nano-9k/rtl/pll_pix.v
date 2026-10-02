// 27 MHz crystal -> 9 MHz pixel clock.
// Gowin rPLL: CLKOUT = FCLKIN * (FBDIV_SEL+1) / (IDIV_SEL+1) = 27 * 1 / 3 = 9 MHz,
// VCO = CLKOUT * ODIV_SEL = 9 * 48 = 432 MHz (valid range 400-1200), PFD = 27/3 = 9 MHz.
// The VCO formula and range are checked by apycula's gowin_pack in CI; the actual
// output frequency is not yet checked on hardware.
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
		.IDIV_SEL(2),
		.FBDIV_SEL(0),
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
