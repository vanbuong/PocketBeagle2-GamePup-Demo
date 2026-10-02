/* SPDX-License-Identifier: GPL-2.0-only */
#include "balbot/imu.h"
#include "tst.h"

static float noise(float sigma)
{
	/* sum of 4 uniforms ~ roughly gaussian, sigma scaled */
	float s = tst_randf() + tst_randf() + tst_randf() + tst_randf();

	return s * sigma * 0.866f;
}

static void estimates_constant_bias(void)
{
	struct bb_gyro_cal c;
	const float bias[3] = {0.02f, -0.015f, 0.005f};
	int r = BB_CAL_RUNNING;

	bb_gyro_cal_init(&c, 1024, 10000);
	for (int i = 0; i < 1024 && r == BB_CAL_RUNNING; i++) {
		float w[3];

		for (int k = 0; k < 3; k++)
			w[k] = bias[k] + noise(0.002f);
		r = bb_gyro_cal_feed(&c, w, 1.0f + noise(0.002f));
	}
	CHECK_EQ(r, BB_CAL_DONE);
	for (int k = 0; k < 3; k++)
		CHECK_NEAR(c.bias[k], bias[k], 3.0 * 0.002 / 32.0 * 2.0);
	CHECK_EQ(c.restarts, 0);
}

static void motion_restarts_window_and_never_accepts_a_biased_result(void)
{
	struct bb_gyro_cal c;
	int r = BB_CAL_RUNNING;
	int i;

	bb_gyro_cal_init(&c, 512, 100000);
	/* first 400 samples fine, then a bump (accel out of range), then quiet again */
	for (i = 0; i < 400; i++) {
		float w[3] = {0.5f, 0.5f, 0.5f}; /* large offset that must NOT be accepted from a disturbed window */
		r = bb_gyro_cal_feed(&c, w, 1.0f);
	}
	{
		float w[3] = {0, 0, 0};

		r = bb_gyro_cal_feed(&c, w, 1.5f); /* bump */
	}
	CHECK_EQ(c.n, 0);
	CHECK_EQ(c.restarts, 1);
	for (i = 0; i < 600 && r == BB_CAL_RUNNING; i++) {
		float w[3] = {0.01f + noise(0.001f), 0.0f + noise(0.001f), 0.0f + noise(0.001f)};

		r = bb_gyro_cal_feed(&c, w, 1.0f);
	}
	CHECK_EQ(r, BB_CAL_DONE);
	CHECK_NEAR(c.bias[0], 0.01, 0.001);
	CHECK_NEAR(c.bias[1], 0.0, 0.001);
}

static void rotating_gyro_is_rejected_by_variance(void)
{
	struct bb_gyro_cal c;
	int r = BB_CAL_RUNNING;

	bb_gyro_cal_init(&c, 256, 3000);
	for (int i = 0; i < 3000 && r == BB_CAL_RUNNING; i++) {
		float w[3] = {0.3f * (float)((i % 20) - 10) / 10.0f, 0, 0}; /* shaking */

		r = bb_gyro_cal_feed(&c, w, 1.0f);
	}
	CHECK_EQ(r, BB_CAL_FAILED); /* never reports DONE on a moving robot, and times out */
	CHECK(c.restarts >= 1);
}

static void timeout_fails_and_state_is_sticky(void)
{
	struct bb_gyro_cal c;
	float w[3] = {0, 0, 0};
	int r = BB_CAL_RUNNING;

	bb_gyro_cal_init(&c, 1000, 50);
	for (int i = 0; i < 60; i++)
		r = bb_gyro_cal_feed(&c, w, 1.0f);
	CHECK_EQ(r, BB_CAL_FAILED);
	CHECK_EQ(bb_gyro_cal_feed(&c, w, 1.0f), BB_CAL_FAILED);
}

int main(void)
{
	RUN(estimates_constant_bias);
	RUN(motion_restarts_window_and_never_accepts_a_biased_result);
	RUN(rotating_gyro_is_rejected_by_variance);
	RUN(timeout_fails_and_state_is_sticky);
	TEST_MAIN_END();
}
