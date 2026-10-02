# PRU firmware (skeleton, not started)

Shared layouts live in `../core/include/balbot/mailbox.h` (host-tested: sizes, offsets, the 4x quadrature table and the
seqlock reader). Planned programs (doc 01, 1.2 and 1.5):

- `pru0_qdec/`: poll both encoder pairs, apply `bb_qdec_step`, publish counts and the IEP timestamp of the last edge
  under the seqlock (`bb_pru0_out`).
- `pru1_pwm/`: 20 kHz PWM and TB6612 IN1/IN2 outputs from `bb_pru1_cmd`; brake if `heartbeat` is stale for > 10 ms
  (`failsafe_active`), drive `MOT_EN_PRU` only while healthy.

Needs the TI PRU compiler (`clpru`) and pin assignments from `../hardware/pinmap.yaml` (all `tbd` today). Nothing here is
written yet because the PRU pads are unconfirmed (docs/balance-robot/07-development-plan.md, risk R1).
