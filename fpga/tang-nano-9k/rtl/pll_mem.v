// 27 MHz crystal -> 148.5 MHz PSRAM memory clock (the IP derives clk_out = 74.25 MHz).
// Gowin rPLL: CLKOUT = FCLKIN * (FBDIV_SEL+1) / (IDIV_SEL+1) = 27 * 11 / 2 = 148.5 MHz,
// VCO = CLKOUT * ODIV_SEL = 148.5 * 4 = 594 MHz (valid range 400-1200), PFD = 13.5 MHz.
// The VCO formula and range are checked by apycula's gowin_pack in CI (PSRAM build is
// not packable without the Gowin IP, so check it with the BSRAM PLL or by hand);
// not yet verified on hardware.
module pll_mem (
	input  wire clkin,    // 27 MHz
	output wire clkout,   // 148.5 MHz
	output wire lock
);
`ifdef SIM
	assign clkout = 1'b0;                    // the simulation IP model does not use it
	reg lk = 1'b0;
	initial #2000 lk = 1'b1;
	assign lock = lk;
`else
	wire clkoutp, clkoutd, clkoutd3;
	rPLL #(
		.FCLKIN("27"),
		.IDIV_SEL(1),
		.FBDIV_SEL(10),
		.ODIV_SEL(4),
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
