/* SPDX-License-Identifier: GPL-2.0-only */
/* Flat C API for the Python simulator: runs the REAL core code (IMU conversion, estimator,
 * speed estimation, balance controller) one 500 Hz step at a time. */
#include <stdlib.h>
#include <string.h>

#include "balbot/balance.h"
#include "balbot/estimator.h"
#include "balbot/imu.h"
#include "balbot/speed.h"

struct bbsim {
	struct bb_imu_cal cal;
	struct bb_cf cf;
	struct bb_kf kf;
	int use_kf;
	struct bb_speed_win sl, sr;
	struct bb_ctrl ctrl;
	struct bb_params p;
	int tick;
	float v_meas;
};

void *bbsim_create(int use_kf, double m_per_count, double cf_tau, double gate_g)
{
	struct bbsim *s = calloc(1, sizeof(*s));

	if (!s)
		return NULL;
	bb_imu_cal_default(&s->cal, 0);
	bb_cf_init(&s->cf, (float)cf_tau, (float)gate_g);
	bb_kf_init(&s->kf, 1e-3f, 1e-5f, (0.5f * BB_PI / 180.0f) * (0.5f * BB_PI / 180.0f), (float)gate_g);
	s->use_kf = use_kf;
	bb_speed_win_init(&s->sl, (float)m_per_count, 0.5f);
	bb_speed_win_init(&s->sr, (float)m_per_count, 0.5f);
	bb_params_default(&s->p);
	bb_ctrl_init(&s->ctrl, &s->p);
	return s;
}

void bbsim_destroy(void *h)
{
	free(h);
}

/* gains: kp,ki,kd (angle), kp,ki (speed), kp,ki (yaw) */
void bbsim_configure(void *h, const double g[7], double u_dz, double theta_max_rad, double kv_ff)
{
	struct bbsim *s = h;

	s->p.kp_angle = (float)g[0];
	s->p.ki_angle = (float)g[1];
	s->p.kd_angle = (float)g[2];
	s->p.kp_speed = (float)g[3];
	s->p.ki_speed = (float)g[4];
	s->p.kp_yaw = (float)g[5];
	s->p.ki_yaw = (float)g[6];
	s->p.u_dz = (float)u_dz;
	s->p.theta_max = (float)theta_max_rad;
	s->p.kv_ff = (float)kv_ff;
	bb_ctrl_init(&s->ctrl, &s->p);
}

void bbsim_set_gyro_bias(void *h, double bias_y, double bias_z)
{
	struct bbsim *s = h;

	s->cal.gyro_bias_rads[1] = (float)bias_y;
	s->cal.gyro_bias_rads[2] = (float)bias_z;
	s->cf.bias = 0.0f;
}

void bbsim_set_intent(void *h, double vx, double wz)
{
	struct bbsim *s = h;

	bb_ctrl_set_intent(&s->ctrl, (float)vx, (float)wz);
}

void bbsim_reset_ctrl(void *h)
{
	struct bbsim *s = h;

	bb_ctrl_reset(&s->ctrl);
}

void bbsim_set_theta_trim(void *h, double trim)
{
	struct bbsim *s = h;

	s->ctrl.theta_trim = (float)trim;
}

/* raw: ax ay az temp gx gy gz (int16). out: u_l u_r pitch rate theta_ref u_bal v_meas yaw_rate */
void bbsim_step(void *h, const int16_t raw[7], int32_t cnt_l, int32_t cnt_r, double vbat, double dt, double out[8])
{
	struct bbsim *s = h;
	struct bb_imu_raw r = {raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6]};
	struct bb_imu_si si;
	struct bb_ctrl_in in;
	struct bb_ctrl_out o;
	float pitch, rate;

	bb_imu_convert(&r, &s->cal, &si);
	if (s->use_kf) {
		bb_kf_update(&s->kf, si.w_rads[1], si.a_g[0], si.a_g[1], si.a_g[2], (float)dt);
		pitch = s->kf.pitch;
		rate = s->kf.rate;
	} else {
		bb_cf_update(&s->cf, si.w_rads[1], si.a_g[0], si.a_g[1], si.a_g[2], (float)dt);
		pitch = s->cf.pitch;
		rate = s->cf.rate;
	}
	if (++s->tick % 5 == 0) { /* 100 Hz speed estimate */
		float vl = bb_speed_win_update(&s->sl, cnt_l, (float)dt * 5.0f);
		float vr = bb_speed_win_update(&s->sr, cnt_r, (float)dt * 5.0f);

		s->v_meas = 0.5f * (vl + vr);
	}
	in.pitch = pitch;
	in.pitch_rate = rate;
	in.yaw_rate = si.w_rads[2];
	in.v_meas = s->v_meas;
	in.vbat = (float)vbat;
	bb_ctrl_step(&s->ctrl, &in, (float)dt, &o);
	out[0] = o.u_l;
	out[1] = o.u_r;
	out[2] = pitch;
	out[3] = rate;
	out[4] = o.theta_ref;
	out[5] = o.u_bal;
	out[6] = s->v_meas;
	out[7] = si.w_rads[2];
}
