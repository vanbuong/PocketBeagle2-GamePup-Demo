/* SPDX-License-Identifier: GPL-2.0-only */
#include "balbot/pid.h"
#include "tst.h"

static void proportional_and_derivative_math(void)
{
	struct bb_pid p;

	bb_pid_init(&p, 2.0f, 0.0f, 0.5f, -100, 100, 100);
	CHECK_NEAR(bb_pid_step(&p, 3.0f, 4.0f, 0.001f), 2.0 * 3.0 + 0.5 * 4.0, 1e-6);
	CHECK_NEAR(bb_pid_step(&p, -1.0f, 0.0f, 0.001f), -2.0, 1e-6);
}

static void integral_accumulates(void)
{
	struct bb_pid p;
	float u = 0;

	bb_pid_init(&p, 0.0f, 10.0f, 0.0f, -100, 100, 100);
	for (int i = 0; i < 1000; i++)
		u = bb_pid_step(&p, 0.5f, 0.0f, 0.001f); /* integral of 10*0.5 over 1 s = 5 */
	CHECK_NEAR(u, 5.0, 1e-3);
	bb_pid_reset(&p);
	CHECK_NEAR(bb_pid_step(&p, 0.0f, 0.0f, 0.001f), 0.0, 1e-9);
}

static void no_derivative_kick_on_setpoint_step(void)
{
	/* the derivative input is the measured rate, so changing the error (setpoint) by a step
	 * moves the output by exactly kp*step and nothing more */
	struct bb_pid p;

	bb_pid_init(&p, 5.0f, 0.0f, 3.0f, -1000, 1000, 1000);
	float u0 = bb_pid_step(&p, 0.0f, 0.2f, 0.002f);
	float u1 = bb_pid_step(&p, 1.0f, 0.2f, 0.002f); /* setpoint step of 1 */

	CHECK_NEAR(u1 - u0, 5.0, 1e-5);
}

static void anti_windup_bounds_integrator_and_recovers(void)
{
	struct bb_pid p;

	bb_pid_init(&p, 1.0f, 50.0f, 0.0f, -2.0f, 2.0f, 1.5f);
	for (int i = 0; i < 5000; i++) /* saturated for 5 s with a constant error */
		bb_pid_step(&p, 3.0f, 0.0f, 0.001f);
	CHECK(p.integ <= 1.5f + 1e-6f);
	CHECK(p.integ >= 0.0f);
	/* error reverses: output leaves saturation within a few ms, not after unwinding seconds */
	int steps = 0;
	float u = 2.0f;

	while (u > 0.0f && steps < 1000) {
		u = bb_pid_step(&p, -1.0f, 0.0f, 0.001f);
		steps++;
	}
	CHECK(steps < 100);
}

static void conditional_integration_freezes_in_saturation(void)
{
	struct bb_pid p;

	bb_pid_init(&p, 10.0f, 5.0f, 0.0f, -1.0f, 1.0f, 100.0f);
	for (int i = 0; i < 1000; i++)
		bb_pid_step(&p, 1.0f, 0.0f, 0.001f); /* u = 10 > 1: saturated from the first step */
	CHECK_NEAR(p.integ, 0.0, 1e-9); /* saturated and error pushing further: integrator frozen */
}

static void output_clamped_both_sides(void)
{
	struct bb_pid p;

	bb_pid_init(&p, 100.0f, 0.0f, 0.0f, -3.0f, 4.0f, 10.0f);
	CHECK_NEAR(bb_pid_step(&p, 1.0f, 0, 0.001f), 4.0, 1e-6);
	CHECK_NEAR(bb_pid_step(&p, -1.0f, 0, 0.001f), -3.0, 1e-6);
}

int main(void)
{
	RUN(proportional_and_derivative_math);
	RUN(integral_accumulates);
	RUN(no_derivative_kick_on_setpoint_step);
	RUN(anti_windup_bounds_integrator_and_recovers);
	RUN(conditional_integration_freezes_in_saturation);
	RUN(output_clamped_both_sides);
	TEST_MAIN_END();
}
