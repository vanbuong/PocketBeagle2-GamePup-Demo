/* SPDX-License-Identifier: GPL-2.0-only */
#include "balbot/speed.h"
#include "tst.h"

#define MPC (0.065 * 3.14159265358979 / 1320.0) /* 0.155 mm per count: 65 mm wheel, 1:30, 4x */

static void window_tracks_constant_speed_within_one_quantum(void)
{
	double speeds[] = {0.02, 0.1, 0.3, 0.6, 1.1};

	for (unsigned k = 0; k < sizeof speeds / sizeof speeds[0]; k++) {
		struct bb_speed_win s;
		double pos = 0.0, v = 0.0;
		float dt = 0.01f;

		bb_speed_win_init(&s, (float)MPC, 1.0f);
		for (int i = 0; i < 200; i++) {
			pos += speeds[k] * dt;
			v = bb_speed_win_update(&s, (int32_t)(pos / MPC), dt);
		}
		CHECK_NEAR(v, speeds[k], MPC / dt + 1e-5); /* one count per window */
	}
}

static void window_handles_reverse_and_wrap(void)
{
	struct bb_speed_win s;

	bb_speed_win_init(&s, 0.001f, 1.0f);
	bb_speed_win_update(&s, 2147483640, 0.01f);
	CHECK_NEAR(bb_speed_win_update(&s, (int32_t)(2147483640u + 20u), 0.01f), 2.0, 1e-4); /* wraps past INT32_MAX */
	bb_speed_win_init(&s, 0.001f, 1.0f);
	bb_speed_win_update(&s, 100, 0.01f);
	CHECK_NEAR(bb_speed_win_update(&s, 90, 0.01f), -1.0, 1e-5);
}

static void window_low_pass(void)
{
	struct bb_speed_win s;

	bb_speed_win_init(&s, 0.001f, 0.2f);
	bb_speed_win_update(&s, 0, 0.01f);
	float v = bb_speed_win_update(&s, 10, 0.01f); /* raw 1.0 m/s */

	CHECK_NEAR(v, 0.2, 1e-5);
}

static void window_staircase_at_low_speed_shows_why_mt_exists(void)
{
	/* 0.02 m/s over a 10 ms window is 1.3 counts: the output alternates between 0 and 1 count */
	struct bb_speed_win s;
	double pos = 0, vmin = 1e9, vmax = -1e9;
	float dt = 0.01f;

	bb_speed_win_init(&s, (float)MPC, 1.0f);
	for (int i = 0; i < 300; i++) {
		pos += 0.02 * dt;
		float v = bb_speed_win_update(&s, (int32_t)(pos / MPC), dt);

		if (i > 5) {
			vmin = v < vmin ? v : vmin;
			vmax = v > vmax ? v : vmax;
		}
	}
	CHECK(vmax - vmin > 0.01); /* visible staircase: ~0.0155 m/s per count */
}

static void mt_accurate_at_low_speed(void)
{
	/* edges arrive every MPC/v seconds; the PRU timestamps the last edge at 200 MHz */
	double speeds[] = {0.01, 0.02, 0.05, 0.15, 0.6};
	const float hz = 200e6f;

	for (unsigned k = 0; k < sizeof speeds / sizeof speeds[0]; k++) {
		struct bb_speed_mt s;
		double v = speeds[k], pos = 0.0, t = 0.0;
		int32_t count = 0;
		uint32_t last_ts = 0;
		float est = 0;

		bb_speed_mt_init(&s, (float)MPC, hz, (uint32_t)(0.2 * hz));
		for (int i = 0; i < 400; i++) { /* sampled every 10 ms */
			double tn = t + 0.01;

			while ((double)(count + 1) * MPC <= v * tn) { /* new edge(s) inside this window */
				count++;
				last_ts = (uint32_t)((double)count * MPC / v * hz);
			}
			pos = v * tn;
			(void)pos;
			t = tn;
			est = bb_speed_mt_update(&s, count, last_ts, (uint32_t)(t * hz));
		}
		CHECK_NEAR(est, v, v * 0.02); /* within 2 % */
	}
}

static void mt_timeout_and_direction(void)
{
	struct bb_speed_mt s;
	const float hz = 200e6f;

	bb_speed_mt_init(&s, 0.001f, hz, (uint32_t)(0.1 * hz));
	bb_speed_mt_update(&s, 0, 0, 0);
	bb_speed_mt_update(&s, 1, (uint32_t)(0.01 * hz), (uint32_t)(0.01 * hz));
	float v = bb_speed_mt_update(&s, 2, (uint32_t)(0.02 * hz), (uint32_t)(0.02 * hz));

	CHECK_NEAR(v, 0.1, 1e-4);
	/* reversing */
	v = bb_speed_mt_update(&s, 1, (uint32_t)(0.03 * hz), (uint32_t)(0.03 * hz));
	CHECK_NEAR(v, -0.1, 1e-4);
	/* no edge for longer than the timeout: speed 0 */
	v = bb_speed_mt_update(&s, 1, (uint32_t)(0.03 * hz), (uint32_t)(0.2 * hz));
	CHECK_NEAR(v, 0.0, 1e-9);
	/* timestamp wrap */
	bb_speed_mt_init(&s, 0.001f, hz, (uint32_t)(0.1 * hz));
	bb_speed_mt_update(&s, 0, 0xFFFFFF00u, 0xFFFFFF00u);
	v = bb_speed_mt_update(&s, 1, 0xFFFFFF00u + 2000000u, 0xFFFFFF00u + 2000000u);
	CHECK_NEAR(v, 0.001 * 200e6 / 2000000.0, 1e-3);
}

int main(void)
{
	RUN(window_tracks_constant_speed_within_one_quantum);
	RUN(window_handles_reverse_and_wrap);
	RUN(window_low_pass);
	RUN(window_staircase_at_low_speed_shows_why_mt_exists);
	RUN(mt_accurate_at_low_speed);
	RUN(mt_timeout_and_direction);
	TEST_MAIN_END();
}
