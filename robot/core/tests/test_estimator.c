/* SPDX-License-Identifier: GPL-2.0-only */
#include "balbot/estimator.h"
#include "balbot/imu.h"
#include "tst.h"

#define DEG (BB_PI / 180.0f)

static void accel_from_pitch(float pitch, float *ax, float *az)
{
	*ax = -sinf(pitch);
	*az = cosf(pitch);
}

static void accel_pitch_sign_convention(void)
{
	float ax, az;

	/* leaning forward (positive pitch): x axis points down, so a_x < 0 */
	accel_from_pitch(10.0f * DEG, &ax, &az);
	CHECK(ax < 0.0f);
	CHECK_NEAR(bb_accel_pitch(ax, az), 10.0 * DEG, 1e-6);
	accel_from_pitch(-20.0f * DEG, &ax, &az);
	CHECK_NEAR(bb_accel_pitch(ax, az), -20.0 * DEG, 1e-6);
}

static void weight_gate(void)
{
	CHECK_NEAR(bb_accel_weight(0, 0, 1.0f, 0.1f), 1.0, 1e-6);
	CHECK_NEAR(bb_accel_weight(0, 0, 1.05f, 0.1f), 0.5, 1e-5);
	CHECK_NEAR(bb_accel_weight(0, 0, 1.2f, 0.1f), 0.0, 1e-6);
	CHECK_NEAR(bb_accel_weight(0.6f, 0.0f, 0.8f, 0.1f), 1.0, 1e-5); /* magnitude, not axis */
}

static void cf_static_tilt_converges(void)
{
	struct bb_cf f;
	float ax, az, dt = 0.001f;

	bb_cf_init(&f, 0.5f, 0.1f);
	accel_from_pitch(8.0f * DEG, &ax, &az);
	for (int i = 0; i < (int)(3.0f * 0.5f / dt * 3.0f); i++) /* 9 tau, started from first sample */
		bb_cf_update(&f, 0.0f, ax, 0.0f, az, dt);
	CHECK_NEAR(f.pitch, 8.0 * DEG, 0.1 * DEG);
}

static void cf_constant_rate_integrates_gyro(void)
{
	struct bb_cf f;
	float ax, az, dt = 0.001f, rate = 90.0f * DEG; /* 90 deg/s for 0.5 s -> 45 deg */

	bb_cf_init(&f, 1e6f, 0.1f); /* accel effectively disabled by the huge tau */
	accel_from_pitch(0.0f, &ax, &az);
	bb_cf_update(&f, 0.0f, ax, 0.0f, az, dt); /* init from accel */
	for (int i = 0; i < 500; i++)
		bb_cf_update(&f, rate, ax, 0.0f, az, dt);
	CHECK_NEAR(f.pitch, 45.0 * DEG, 0.45 * DEG); /* 1 % */
	CHECK_NEAR(f.rate, rate, 1e-6);
}

static void cf_ignores_accel_spike(void)
{
	struct bb_cf f;
	float ax, az, dt = 0.001f;

	bb_cf_init(&f, 0.5f, 0.1f);
	accel_from_pitch(0.0f, &ax, &az);
	for (int i = 0; i < 2000; i++)
		bb_cf_update(&f, 0.0f, ax, 0.0f, az, dt);
	/* 1 g forward acceleration pulse for 100 ms: |a| = 1.41 g, would read 45 deg */
	for (int i = 0; i < 100; i++)
		bb_cf_update(&f, 0.0f, -1.0f, 0.0f, 1.0f, dt);
	CHECK(fabsf(f.pitch) < 2.0f * DEG);
}

static void cf_removes_configured_bias(void)
{
	struct bb_cf f;
	float ax, az;

	bb_cf_init(&f, 1e6f, 0.1f);
	f.bias = 0.02f;
	accel_from_pitch(0.0f, &ax, &az);
	bb_cf_update(&f, 0.02f, ax, 0.0f, az, 0.001f);
	for (int i = 0; i < 1000; i++)
		bb_cf_update(&f, 0.02f, ax, 0.0f, az, 0.001f);
	CHECK_NEAR(f.pitch, 0.0, 1e-4);
	CHECK_NEAR(f.rate, 0.0, 1e-6);
}

static void kf_converges_and_estimates_bias(void)
{
	struct bb_kf f;
	float ax, az, dt = 0.001f;
	const float true_bias = 0.02f; /* rad/s */

	bb_kf_init(&f, 1e-3f, 1e-5f, (0.5f * DEG) * (0.5f * DEG), 0.1f);
	accel_from_pitch(5.0f * DEG, &ax, &az);
	for (int i = 0; i < 60000; i++) /* 60 s, noiseless, robot held at 5 deg */
		bb_kf_update(&f, true_bias, ax, 0.0f, az, dt);
	CHECK_NEAR(f.pitch, 5.0 * DEG, 0.05 * DEG);
	CHECK_NEAR(f.bias, true_bias, 0.002);
	CHECK_NEAR(f.rate, 0.0, 0.002);
}

static void kf_resists_accel_disturbance(void)
{
	struct bb_kf f;
	float ax, az, dt = 0.001f;

	bb_kf_init(&f, 1e-3f, 1e-5f, (0.5f * DEG) * (0.5f * DEG), 0.1f);
	accel_from_pitch(0.0f, &ax, &az);
	for (int i = 0; i < 3000; i++)
		bb_kf_update(&f, 0.0f, ax, 0.0f, az, dt);
	for (int i = 0; i < 100; i++)
		bb_kf_update(&f, 0.0f, -1.0f, 0.0f, 1.0f, dt);
	CHECK(fabsf(f.pitch) < 2.0f * DEG);
}

static void numeric_health_long_random_run(void)
{
	struct bb_kf k;
	struct bb_cf c;
	float dt = 0.001f;

	bb_kf_init(&k, 1e-3f, 1e-5f, (0.5f * DEG) * (0.5f * DEG), 0.1f);
	bb_cf_init(&c, 0.5f, 0.1f);
	for (int i = 0; i < 2000000; i++) {
		float p = 0.3f * sinf((float)i * 0.0005f);
		float ax = -sinf(p) + 0.05f * tst_randf();
		float az = cosf(p) + 0.05f * tst_randf();
		float g = 0.3f * 0.0005f / dt * cosf((float)i * 0.0005f) + 0.01f * tst_randf();

		if ((i % 5000) < 20) { /* disturbances: wild accel readings */
			ax = 3.0f * tst_randf();
			az = 3.0f * tst_randf();
		}
		bb_kf_update(&k, g, ax, 0.0f, az, dt);
		bb_cf_update(&c, g, ax, 0.0f, az, dt);
		if (!(fabsf(k.pitch) < 3.5f) || !isfinite(k.P[0][0]) || k.P[0][0] < 0.0f || k.P[1][1] < 0.0f ||
		    !isfinite(c.pitch)) {
			CHECK(0);
			return;
		}
	}
	CHECK(1);
}

int main(void)
{
	RUN(accel_pitch_sign_convention);
	RUN(weight_gate);
	RUN(cf_static_tilt_converges);
	RUN(cf_constant_rate_integrates_gyro);
	RUN(cf_ignores_accel_spike);
	RUN(cf_removes_configured_bias);
	RUN(kf_converges_and_estimates_bias);
	RUN(kf_resists_accel_disturbance);
	RUN(numeric_health_long_random_run);
	TEST_MAIN_END();
}
