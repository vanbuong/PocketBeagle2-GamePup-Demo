/* SPDX-License-Identifier: GPL-2.0-only */
#include "balbot/estimator.h"

#include <math.h>
#include <string.h>

float bb_accel_pitch(float ax_g, float az_g)
{
	return atan2f(-ax_g, az_g);
}

float bb_accel_weight(float ax_g, float ay_g, float az_g, float gate_g)
{
	float mag = sqrtf(ax_g * ax_g + ay_g * ay_g + az_g * az_g);
	float dev = fabsf(mag - 1.0f);
	float w = 1.0f - dev / gate_g;

	return w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
}

void bb_cf_init(struct bb_cf *f, float tau_s, float gate_g)
{
	memset(f, 0, sizeof(*f));
	f->tau = tau_s;
	f->gate_g = gate_g;
}

void bb_cf_update(struct bb_cf *f, float gyro_y, float ax_g, float ay_g, float az_g, float dt)
{
	float w = bb_accel_weight(ax_g, ay_g, az_g, f->gate_g);
	float acc = bb_accel_pitch(ax_g, az_g);
	float alpha = f->tau / (f->tau + dt);

	f->rate = gyro_y - f->bias;
	if (!f->started) {
		if (w > 0.5f) {
			f->pitch = acc;
			f->started = 1;
		}
		return;
	}
	{
		float pred = f->pitch + f->rate * dt;
		float k = (1.0f - alpha) * w; /* accel gain, 0 when the accelerometer is not trustworthy */

		f->pitch = pred + k * (acc - pred);
	}
}

void bb_kf_init(struct bb_kf *f, float q_pitch, float q_bias, float r_acc, float gate_g)
{
	memset(f, 0, sizeof(*f));
	f->q_pitch = q_pitch;
	f->q_bias = q_bias;
	f->r_acc = r_acc;
	f->r_inflate = 1e4f;
	f->gate_g = gate_g;
	f->P[0][0] = 1.0f;
	f->P[1][1] = 1e-2f;
}

void bb_kf_update(struct bb_kf *f, float gyro_y, float ax_g, float ay_g, float az_g, float dt)
{
	float w = bb_accel_weight(ax_g, ay_g, az_g, f->gate_g);
	float acc = bb_accel_pitch(ax_g, az_g);
	float r, s, k0, k1, y, p00, p01;

	if (!f->started) {
		if (w > 0.5f) {
			f->pitch = acc;
			f->started = 1;
		}
		f->rate = gyro_y;
		return;
	}
	/* predict */
	f->rate = gyro_y - f->bias;
	f->pitch += dt * f->rate;
	f->P[0][0] += dt * (dt * f->P[1][1] - f->P[0][1] - f->P[1][0] + f->q_pitch);
	f->P[0][1] -= dt * f->P[1][1];
	f->P[1][0] -= dt * f->P[1][1];
	f->P[1][1] += f->q_bias * dt;
	/* update, measurement noise grows smoothly as the accel becomes untrustworthy */
	r = f->r_acc * (1.0f + (f->r_inflate - 1.0f) * (1.0f - w));
	s = f->P[0][0] + r;
	k0 = f->P[0][0] / s;
	k1 = f->P[1][0] / s;
	y = acc - f->pitch;
	f->pitch += k0 * y;
	f->bias += k1 * y;
	p00 = f->P[0][0];
	p01 = f->P[0][1];
	f->P[0][0] -= k0 * p00;
	f->P[0][1] -= k0 * p01;
	f->P[1][0] -= k1 * p00;
	f->P[1][1] -= k1 * p01;
	if (f->P[0][0] < 0.0f)
		f->P[0][0] = 0.0f;
	if (f->P[1][1] < 0.0f)
		f->P[1][1] = 0.0f;
	f->rate = gyro_y - f->bias;
}
