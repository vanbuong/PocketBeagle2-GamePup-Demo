// 27 MHz crystal -> 148.5 MHz PSRAM memory clock (the IP derives clk_out = 74.25 MHz).
// Gowin rPLL: VCO = 27 * (21+1) / (0+1) = 594 MHz, / ODIV 4 = 148.5 MHz.
// NOT yet verified in Gowin EDA - confirm with the IP generator (same values as the
// Sipeed-style PSRAM example: 148.5 MHz memory_clk).
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
		.IDIV_SEL(0),
		.FBDIV_SEL(21),
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
