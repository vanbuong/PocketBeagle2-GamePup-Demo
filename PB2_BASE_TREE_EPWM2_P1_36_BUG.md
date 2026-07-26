# PocketBeagle 2 base-tree `epwm2` pinctrl unexpectedly claims P1.36

## Summary

On PocketBeagle 2, enabling the base-tree `epwm2` controller for a peripheral
that uses only channel B causes the controller's inherited default pinctrl to
claim P1.36 for channel A as well. P1.36 is an expansion-header/mikroBUS signal,
so this prevents an otherwise unrelated GPIO consumer from using that pin.

This was found on Armbian with kernel `6.12.49-vendor-k3-beagle` while enabling
the GamePup A4 buzzer on `EHRPWM2_B` at P1.33 and an OLED C Click whose D/C
signal is connected to the mikroBUS PWM pin at P1.36.

## Expected behavior

An overlay that enables `epwm2` and explicitly muxes only `EHRPWM2_B` on P1.33
should not also reserve P1.36 (`EHRPWM2_A`). P1.36 should remain available to
the expansion-header consumer selected by the overlay.

## Actual behavior

The PB2 base device tree associates `epwm2` with `epwm2-default-pins`, which
contains the following mux entry:

```dts
epwm2_default_pins: epwm2-default-pins {
	/* P1.36 B17: EHRPWM2_A */
	pinctrl-single,pins = <0x01e8 0x010008>;
};
```

When an overlay changes only `&epwm2 { status = "okay"; };`, that inherited
default state becomes active. The live pinctrl state then shows P1.36 claimed
by the PWM controller even though the overlay's PWM user is channel B on
P1.33:

```text
pin 122 (PIN122): 23020000.pwm (GPIO UNCLAIMED)
                  function epwm2-default-pins
                  group epwm2-default-pins
```

The PB2 GPIO metadata identifies the conflicting GPIO route as P1.36 B17,
`main_gpio1` line 28. The base tree's GPIO pin group confirms that it uses the
same pad offset and disables the alternate P1.36 route:

```dts
P1_36_B17_gpio: P1-36-B17-gpio-pins {
	pinctrl-single,pins = <
		0x01e8 0x054007
		0x00e0 0x200007
	>;
};
```

Requesting `main_gpio1` line 28 therefore fails or cannot control the physical
pin until the inherited PWM pinctrl state is removed.

## Minimal reproduction

1. Boot PocketBeagle 2 with its normal base DTB.
2. Apply an overlay that enables `&epwm2` for channel B and places
   `EHRPWM2_B` on P1.33, but does not override the controller's inherited
   `pinctrl-0`.
3. Reboot.
4. Inspect the pin owner:

   ```sh
   sudo grep 'pin 122 ' /sys/kernel/debug/pinctrl/f4000.pinctrl-pinctrl-single/pinmux-pins
   ```

5. Observe that `23020000.pwm` owns P1.36 through
   `epwm2-default-pins`, despite only channel B being required.

## Working overlay-side workaround

Clear the controller's inherited default pinctrl when the consumer supplies
the actual channel-B pinctrl:

```dts
&epwm2 {
	status = "okay";
	pinctrl-names = "default";
	pinctrl-0 = <>;
};

gamepup-buzzer {
	compatible = "pwm-beeper";
	pinctrl-names = "default";
	pinctrl-0 = <&gamepup_buzzer_pwm_pin>; /* P1.33 / EHRPWM2_B */
	pwms = <&epwm2 1 250000 0>;
};
```

After this override, the OLED consumer can mux P1.36 as GPIO1_28 while the
buzzer continues to use channel B on P1.33.

## Suggested base-tree discussion

Expansion-header pins generally should not be claimed merely by enabling a
controller when the selected consumer may use a different channel or route.
Possible fixes to discuss with PB2 device-tree maintainers are:

1. Remove `pinctrl-0 = <&epwm2_default_pins>` from the base `epwm2` controller
   and require each enabled consumer/overlay to select its desired PWM route.
2. Keep the current default for compatibility, but document that any overlay
   using only channel B must explicitly replace or clear the inherited
   `pinctrl-0`.
3. Provide channel/route-specific overlay fragments rather than one controller
   default that reserves an exposed expansion pin.

## Scope and nuance

The base tree does not claim P1.36 while `epwm2` remains disabled. The conflict
is triggered by normal device-tree overlay composition: enabling `epwm2`
activates a base-tree pinctrl choice that may be unrelated to the channel the
overlay intends to use. This makes it a base-tree default/inheritance issue,
with an available overlay-side workaround.
