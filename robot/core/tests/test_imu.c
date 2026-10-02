/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "balbot/imu.h"
#include "tst.h"

static void init_sequence_is_golden(void)
{
	size_t n;
	const struct bb_imu_op *s = bb_mpu6500_init_seq(&n);
	/* order of register writes, values, as documented in doc 04 section 4.3 */
	static const uint8_t regs[] = {0x6A, 0x6B, 0x6A, 0x68, 0x6B, 0x6C, 0x1A, 0x1B, 0x1C, 0x1D, 0x19, 0x37, 0x38};
	static const uint8_t vals[] = {0x10, 0x80, 0x10, 0x07, 0x01, 0x00, 0x01, 0x10, 0x08, 0x01, 0x00, 0x30, 0x01};
	size_t w = 0;
	int who = 0, clocks = 0, discard = 0;
	unsigned khz[2] = {0, 0};

	for (size_t i = 0; i < n; i++) {
		if (s[i].kind == BB_IMU_OP_WRITE) {
			CHECK(w < sizeof regs);
			CHECK_EQ(s[i].reg, regs[w]);
			CHECK_EQ(s[i].val, vals[w]);
			w++;
		} else if (s[i].kind == BB_IMU_OP_EXPECT) {
			CHECK_EQ(s[i].reg, 0x75);
			CHECK_EQ(s[i].val, 0x70);
			who++;
			CHECK_EQ(w, 5); /* after PWR_MGMT_1 = 0x01 (5th write), before configuration */
		} else if (s[i].kind == BB_IMU_OP_SPI_KHZ) {
			if (clocks < 2)
				khz[clocks] = s[i].arg;
			clocks++;
		} else if (s[i].kind == BB_IMU_OP_DISCARD) {
			discard = s[i].arg;
		}
	}
	CHECK_EQ(w, sizeof regs);
	CHECK_EQ(who, 1);
	CHECK_EQ(clocks, 2);
	CHECK_EQ(khz[0], 1000); /* config registers: <= 1 MHz */
	CHECK_EQ(khz[1], 4000); /* sensor reads */
	CHECK_EQ(discard, 50);
	/* the reset write must be followed by a 100 ms wait */
	CHECK_EQ(s[3].reg, 0x6B);
	CHECK_EQ(s[3].delay_ms, 100);
	/* the clock is lowered BEFORE the first register write */
	CHECK_EQ(s[0].kind, BB_IMU_OP_SPI_KHZ);
}

static void parse_burst(void)
{
	uint8_t rx[15] = {0xFF, 0x20, 0x00, 0xFF, 0xFE, 0x7F, 0xFF, 0x01, 0x4D, 0x80, 0x00, 0x00, 0x01, 0xFF, 0xFF};
	struct bb_imu_raw r;

	CHECK_EQ(bb_mpu6500_parse(rx, &r), 0);
	CHECK_EQ(r.ax, 0x2000);
	CHECK_EQ(r.ay, -2);
	CHECK_EQ(r.az, 32767);
	CHECK_EQ(r.temp, 0x014D);
	CHECK_EQ(r.gx, -32768);
	CHECK_EQ(r.gy, 1);
	CHECK_EQ(r.gz, -1);
}

static void conversion_scales(void)
{
	struct bb_imu_cal cal;
	struct bb_imu_si si;
	struct bb_imu_raw r = {0, 0, 8192, 0, 0, 0, 0};

	bb_imu_cal_default(&cal, 0);
	bb_imu_convert(&r, &cal, &si);
	CHECK_NEAR(si.a_g[2], 1.0, 1e-6);
	r.az = 0;
	r.gx = 1; /* 1 LSB */
	bb_imu_convert(&r, &cal, &si);
	CHECK_NEAR(si.w_rads[0], (1.0 / 32.8) * 3.14159265 / 180.0, 1e-9);
	r.gx = 16400; /* 500 deg/s */
	bb_imu_convert(&r, &cal, &si);
	CHECK_NEAR(si.w_rads[0], 500.0 * 3.14159265 / 180.0, 1e-3);
	r.temp = 0;
	bb_imu_convert(&r, &cal, &si);
	CHECK_NEAR(si.temp_c, 21.0, 1e-4);
	/* bias and offsets */
	cal.gyro_bias_rads[0] = 0.01f;
	cal.accel_offset_g[0] = 0.05f;
	cal.accel_scale[0] = 2.0f;
	r.gx = 0;
	r.ax = 819; /* 0.1 g */
	bb_imu_convert(&r, &cal, &si);
	CHECK_NEAR(si.w_rads[0], -0.01, 1e-7);
	CHECK_NEAR(si.a_g[0], (819.0 / 8192.0 - 0.05) * 2.0, 1e-5);
}

static void det3(const int8_t R[3][3], int *d)
{
	*d = R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) - R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) +
	     R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);
}

static void all_24_orientations_are_proper_and_unique(void)
{
	int8_t all[BB_AXIS_ORIENTATIONS][3][3];

	for (int i = 0; i < BB_AXIS_ORIENTATIONS; i++) {
		int d;

		CHECK_EQ(bb_axis_rotation(i, all[i]), 0);
		det3(all[i], &d);
		CHECK_EQ(d, 1);
		for (int r = 0; r < 3; r++) {
			int rs = 0, cs = 0;

			for (int c = 0; c < 3; c++) {
				rs += all[i][r][c] * all[i][r][c];
				cs += all[i][c][r] * all[i][c][r];
			}
			CHECK_EQ(rs, 1);
			CHECK_EQ(cs, 1);
		}
		for (int j = 0; j < i; j++)
			CHECK(memcmp(all[i], all[j], sizeof all[i]) != 0);
	}
	int8_t tmp[3][3];

	CHECK_EQ(bb_axis_rotation(-1, tmp), -1);
	CHECK_EQ(bb_axis_rotation(24, tmp), -1);
}

static void every_orientation_maps_gravity_to_plus_z_for_a_matching_mounting(void)
{
	/* For each orientation R (sensor->body), a sensor that sees gravity along R^T * (0,0,1)
	 * must report +1 g on the body z axis. */
	for (int i = 0; i < BB_AXIS_ORIENTATIONS; i++) {
		struct bb_imu_cal cal;
		struct bb_imu_raw raw = {0};
		struct bb_imu_si si;
		float s[3];

		bb_imu_cal_default(&cal, i);
		for (int c = 0; c < 3; c++)
			s[c] = (float)cal.R[2][c]; /* R^T * (0,0,1) = third row of R */
		raw.ax = (int16_t)(s[0] * 8192.0f);
		raw.ay = (int16_t)(s[1] * 8192.0f);
		raw.az = (int16_t)(s[2] * 8192.0f);
		bb_imu_convert(&raw, &cal, &si);
		CHECK_NEAR(si.a_g[0], 0.0, 1e-5);
		CHECK_NEAR(si.a_g[1], 0.0, 1e-5);
		CHECK_NEAR(si.a_g[2], 1.0, 1e-5);
	}
}

static void health_faults(void)
{
	struct bb_imu_health h;
	uint8_t d[14], z[14] = {0}, f[14];

	memset(f, 0xFF, sizeof f);
	bb_imu_health_init(&h);
	for (int i = 0; i < 100; i++) {
		for (int j = 0; j < 14; j++)
			d[j] = (uint8_t)(i * 3 + j);
		CHECK_EQ(bb_imu_health_update(&h, d), 0);
	}
	CHECK(!h.fault);
	CHECK_EQ(bb_imu_health_update(&h, z), 1);
	CHECK_EQ(bb_imu_health_update(&h, f), 1);
	CHECK(!h.fault); /* two bad frames are tolerated */
	for (int j = 0; j < 14; j++)
		d[j] = (uint8_t)j;
	bb_imu_health_update(&h, d);
	CHECK(!h.fault);
	bb_imu_health_update(&h, z);
	bb_imu_health_update(&h, z);
	CHECK(!h.fault);
	bb_imu_health_update(&h, z);
	CHECK(h.fault); /* three in a row */

	/* stuck sensor: identical frames */
	bb_imu_health_init(&h);
	for (int j = 0; j < 14; j++)
		d[j] = (uint8_t)(j + 1);
	int bad = 0;

	for (int i = 0; i < 60; i++)
		bad += bb_imu_health_update(&h, d);
	CHECK(h.fault);
	CHECK(bad >= 3);
}

static void dt_filter(void)
{
	struct bb_dt_filter f;
	uint32_t t = 1000;

	bb_dt_init(&f, 0.001f);
	for (int i = 0; i < 2000; i++) {
		/* +-100 us jitter around 1 ms, average exactly 1 ms */
		t += 1000 + ((i & 1) ? 100 : -100);
		bb_dt_update(&f, t);
	}
	CHECK_NEAR(f.dt, 0.001, 1e-5);
	CHECK_EQ(f.missed, 0);
	/* a missed DRDY: 2 ms gap */
	t += 2000;
	bb_dt_update(&f, t);
	CHECK_EQ(f.missed, 1);
	CHECK(f.dt < 0.0011f); /* the filter is clamped and slow */
	/* wrap of the microsecond counter is harmless */
	bb_dt_init(&f, 0.001f);
	bb_dt_update(&f, 0xFFFFFF00u);
	bb_dt_update(&f, 0xFFFFFF00u + 1000u);
	CHECK_NEAR(f.dt, 0.001, 1e-6);
	CHECK_EQ(f.missed, 0);
	/* absurd gap is clamped to 2x nominal */
	bb_dt_init(&f, 0.001f);
	bb_dt_update(&f, 0);
	bb_dt_update(&f, 5000000u);
	CHECK(f.dt <= 0.00102f);
}

int main(void)
{
	RUN(init_sequence_is_golden);
	RUN(parse_burst);
	RUN(conversion_scales);
	RUN(all_24_orientations_are_proper_and_unique);
	RUN(every_orientation_maps_gravity_to_plus_z_for_a_matching_mounting);
	RUN(health_faults);
	RUN(dt_filter);
	TEST_MAIN_END();
}
