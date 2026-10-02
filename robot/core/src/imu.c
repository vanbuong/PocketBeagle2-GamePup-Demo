/* SPDX-License-Identifier: GPL-2.0-only */
#include "balbot/imu.h"

#include <math.h>
#include <string.h>

static const struct bb_imu_op init_seq[] = {
	/* 1. delay after power-up, SPI at <= 1 MHz for configuration registers */
	{BB_IMU_OP_SPI_KHZ, 0, 0, 0, 1000},
	{BB_IMU_OP_DELAY, 0, 0, 100, 0},
	{BB_IMU_OP_WRITE, MPU_REG_USER_CTRL, 0x10, 0, 0},            /* I2C_IF_DIS */
	{BB_IMU_OP_WRITE, MPU_REG_PWR_MGMT_1, 0x80, 100, 0},         /* DEVICE_RESET */
	{BB_IMU_OP_WRITE, MPU_REG_USER_CTRL, 0x10, 0, 0},            /* reset re-enables I2C */
	{BB_IMU_OP_WRITE, MPU_REG_SIGNAL_PATH_RESET, 0x07, 100, 0},
	{BB_IMU_OP_WRITE, MPU_REG_PWR_MGMT_1, 0x01, 10, 0},          /* PLL, X gyro */
	{BB_IMU_OP_EXPECT, MPU_REG_WHO_AM_I, MPU6500_WHO_AM_I_VALUE, 0, 0},
	{BB_IMU_OP_WRITE, MPU_REG_PWR_MGMT_2, 0x00, 0, 0},
	{BB_IMU_OP_WRITE, MPU_REG_CONFIG, 0x01, 0, 0},               /* gyro DLPF 184 Hz */
	{BB_IMU_OP_WRITE, MPU_REG_GYRO_CONFIG, 0x10, 0, 0},          /* +-1000 dps */
	{BB_IMU_OP_WRITE, MPU_REG_ACCEL_CONFIG, 0x08, 0, 0},         /* +-4 g */
	{BB_IMU_OP_WRITE, MPU_REG_ACCEL_CONFIG2, 0x01, 0, 0},        /* accel DLPF 218 Hz */
	{BB_IMU_OP_WRITE, MPU_REG_SMPLRT_DIV, 0x00, 0, 0},           /* 1 kHz */
	{BB_IMU_OP_WRITE, MPU_REG_INT_PIN_CFG, 0x30, 0, 0},          /* latch, any-read clear */
	{BB_IMU_OP_WRITE, MPU_REG_INT_ENABLE, 0x01, 0, 0},           /* RAW_RDY_EN */
	{BB_IMU_OP_SPI_KHZ, 0, 0, 0, 4000},                          /* sensor reads at 4 MHz */
	{BB_IMU_OP_DISCARD, 0, 0, 0, 50},
};

const struct bb_imu_op *bb_mpu6500_init_seq(size_t *n)
{
	*n = sizeof(init_seq) / sizeof(init_seq[0]);
	return init_seq;
}

static int16_t be16(const uint8_t *p)
{
	return (int16_t)(uint16_t)((p[0] << 8) | p[1]);
}

int bb_mpu6500_parse(const uint8_t rx[15], struct bb_imu_raw *o)
{
	if (!rx || !o)
		return -1;
	o->ax = be16(&rx[1]);
	o->ay = be16(&rx[3]);
	o->az = be16(&rx[5]);
	o->temp = be16(&rx[7]);
	o->gx = be16(&rx[9]);
	o->gy = be16(&rx[11]);
	o->gz = be16(&rx[13]);
	return 0;
}

int bb_axis_rotation(int idx, int8_t R[3][3])
{
	static const int perm[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
	int k = 0;

	if (idx < 0 || idx >= BB_AXIS_ORIENTATIONS)
		return -1;
	for (int p = 0; p < 6; p++) {
		for (int s = 0; s < 8; s++) {
			int8_t m[3][3] = {{0}};
			int det;

			for (int row = 0; row < 3; row++)
				m[row][perm[p][row]] = (s >> row) & 1 ? -1 : 1;
			det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
			      m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
			      m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
			if (det != 1)
				continue;
			if (k == idx) {
				memcpy(R, m, sizeof(m));
				return 0;
			}
			k++;
		}
	}
	return -1;
}

void bb_imu_cal_default(struct bb_imu_cal *c, int orientation_idx)
{
	memset(c, 0, sizeof(*c));
	bb_axis_rotation(orientation_idx, c->R);
	for (int i = 0; i < 3; i++)
		c->accel_scale[i] = 1.0f;
}

void bb_imu_convert(const struct bb_imu_raw *raw, const struct bb_imu_cal *cal, struct bb_imu_si *out)
{
	float a[3] = {raw->ax / MPU_ACCEL_LSB_PER_G, raw->ay / MPU_ACCEL_LSB_PER_G, raw->az / MPU_ACCEL_LSB_PER_G};
	float w[3] = {raw->gx / MPU_GYRO_LSB_PER_DPS * (BB_PI / 180.0f), raw->gy / MPU_GYRO_LSB_PER_DPS * (BB_PI / 180.0f),
		      raw->gz / MPU_GYRO_LSB_PER_DPS * (BB_PI / 180.0f)};

	for (int i = 0; i < 3; i++) {
		a[i] = (a[i] - cal->accel_offset_g[i]) * cal->accel_scale[i];
		out->a_g[i] = 0;
		out->w_rads[i] = 0;
	}
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++) {
			out->a_g[r] += cal->R[r][c] * a[c];
			out->w_rads[r] += cal->R[r][c] * w[c];
		}
	for (int i = 0; i < 3; i++)
		out->w_rads[i] -= cal->gyro_bias_rads[i]; /* bias is stored in body frame */
	out->temp_c = raw->temp / MPU_TEMP_LSB_PER_C + 21.0f;
}

void bb_imu_health_init(struct bb_imu_health *h)
{
	memset(h, 0, sizeof(*h));
}

int bb_imu_health_update(struct bb_imu_health *h, const uint8_t data14[14])
{
	int all0 = 1, allff = 1, bad;

	for (int i = 0; i < 14; i++) {
		all0 &= data14[i] == 0x00;
		allff &= data14[i] == 0xFF;
	}
	if (h->have_last && memcmp(h->last, data14, 14) == 0)
		h->same_count++;
	else
		h->same_count = 0;
	memcpy(h->last, data14, 14);
	h->have_last = 1;
	bad = all0 || allff || h->same_count >= 50;
	if (bad) {
		h->bad_run++;
		h->bad_total++;
		if (h->bad_run >= 3)
			h->fault = 1;
	} else {
		h->bad_run = 0;
	}
	return bad;
}

void bb_dt_init(struct bb_dt_filter *f, float nominal_s)
{
	memset(f, 0, sizeof(*f));
	f->nominal = nominal_s;
	f->dt = nominal_s;
}

float bb_dt_update(struct bb_dt_filter *f, uint32_t now_us)
{
	if (f->started) {
		float d = (float)(uint32_t)(now_us - f->last_us) * 1e-6f;

		if (d > 1.5f * f->nominal)
			f->missed++;
		if (d < 0.5f * f->nominal)
			d = 0.5f * f->nominal;
		if (d > 2.0f * f->nominal)
			d = 2.0f * f->nominal;
		f->dt += 0.01f * (d - f->dt);
	}
	f->started = 1;
	f->last_us = now_us;
	return f->dt;
}

void bb_gyro_cal_init(struct bb_gyro_cal *c, uint32_t window, uint32_t timeout)
{
	memset(c, 0, sizeof(*c));
	c->window = window;
	c->timeout = timeout;
	c->max_std_rads = 0.5f * (BB_PI / 180.0f);
	c->a_min_g = 0.97f;
	c->a_max_g = 1.03f;
}

static void cal_restart(struct bb_gyro_cal *c)
{
	c->n = 0;
	memset(c->sum, 0, sizeof(c->sum));
	memset(c->sumsq, 0, sizeof(c->sumsq));
	c->restarts++;
}

int bb_gyro_cal_feed(struct bb_gyro_cal *c, const float w_rads[3], float accel_mag_g)
{
	if (c->state != BB_CAL_RUNNING)
		return c->state;
	c->total++;
	if (accel_mag_g < c->a_min_g || accel_mag_g > c->a_max_g) {
		cal_restart(c);
	} else {
		for (int i = 0; i < 3; i++) {
			c->sum[i] += (double)w_rads[i];
			c->sumsq[i] += (double)w_rads[i] * (double)w_rads[i];
		}
		c->n++;
		if (c->n >= c->window) {
			int still = 1;

			for (int i = 0; i < 3; i++) {
				double mean = c->sum[i] / (double)c->n;
				double var = c->sumsq[i] / (double)c->n - mean * mean;

				if (var < 0)
					var = 0;
				if (sqrt(var) > (double)c->max_std_rads)
					still = 0;
				c->bias[i] = (float)mean;
			}
			if (still) {
				c->state = BB_CAL_DONE;
				return c->state;
			}
			cal_restart(c);
		}
	}
	if (c->total >= c->timeout)
		c->state = BB_CAL_FAILED;
	return c->state;
}
