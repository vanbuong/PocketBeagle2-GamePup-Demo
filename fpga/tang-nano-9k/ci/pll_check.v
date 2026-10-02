// Only used by CI: instantiate both PLLs of the design so apycula's gowin_pack validates
// their rPLL parameters (VCO / PFD range) - the PSRAM build cannot be packed without the
// Gowin IP, so its PLL would otherwise never be checked.
module pll_check (
	input  wire XTAL_IN,
	output wire PIX,
	output wire MEM
);
	wire lock_pix, lock_mem;
	pll_pix u_pix (.clkin(XTAL_IN), .clkout(PIX), .lock(lock_pix));
	pll_mem u_mem (.clkin(XTAL_IN), .clkout(MEM), .lock(lock_mem));
endmodule
