/* SPDX-License-Identifier: GPL-2.0-only */
#include "balbot/balance.h"

#include <math.h>
#include <string.h>

#include "balbot/imu.h"

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float approach(float cur, float target, float max_step)
{
	float d = target - cur;

	if (d > max_step)
		return cur + max_step;
	if (d < -max_step)
		return cur - max_step;
	return target;
}

void bb_params_default(struct bb_params *p)
{
	/* LQR-derived cascade for the example model in sim/plant.py (docs 05/08): Kp, Kd from the
	 * angle/rate gains, kp_speed = Kv/Kp, ki_speed = Kx/Kp, kv_ff = Ke/r. A starting point for
	 * THIS robot only: re-derive from measured parameters (sim/lqr_design.py). */
	p->kp_angle = 60.0f;
	p->ki_angle = 0.0f;
	p->kd_angle = 7.5f;
	p->kp_speed = 0.35f;
	p->ki_speed = 0.017f;
	p->kv_ff = 10.2f; /* Ke / wheel radius of the example robot */
	p->theta_max = 10.0f * BB_PI / 180.0f;
	p->kp_yaw = 1.0f;
	p->ki_yaw = 2.0f;
	p->u_turn_max_frac = 0.3f;
	p->v_max = 0.6f;
	p->w_max = 2.5f;
	p->a_max = 0.8f;
	p->alpha_max = 6.0f;
	p->u_max_frac = 0.95f;
	p->u_slew_v_per_s = 400.0f;
	p->u_dz = 0.0f;
	p->u_dz_eps = 0.3f;
	p->headroom_start = 0.8f;
	p->speed_div = 5;
	p->vbat_lpf_hz = 5.0f;
	p->vbat_nom = 11.1f;
}

void bb_ctrl_reset(struct bb_ctrl *c)
{
	bb_pid_reset(&c->angle);
	bb_pid_reset(&c->speed);
	bb_pid_reset(&c->yaw);
	c->v_cmd = c->w_cmd = 0.0f;
	c->v_lim = c->w_lim = 0.0f;
	c->theta_ref = 0.0f;
	c->u_turn = 0.0f;
	c->scale = 1.0f;
	c->u_l_prev = c->u_r_prev = 0.0f;
	c->div_cnt = 0;
	c->started = 0;
}

void bb_ctrl_init(struct bb_ctrl *c, const struct bb_params *p)
{
	memset(c, 0, sizeof(*c));
	c->p = *p;
	bb_pid_init(&c->angle, p->kp_angle, p->ki_angle, p->kd_angle, -1e6f, 1e6f, 0.3f * p->vbat_nom);
	bb_pid_init(&c->speed, p->kp_speed, p->ki_speed, 0.0f, -p->theta_max, p->theta_max, p->theta_max);
	bb_pid_init(&c->yaw, p->kp_yaw, p->ki_yaw, 0.0f, -1e6f, 1e6f, p->u_turn_max_frac * p->vbat_nom);
	c->vbat_f = p->vbat_nom;
	c->scale = 1.0f;
}

void bb_ctrl_set_intent(struct bb_ctrl *c, float vx, float wz)
{
	c->v_cmd = clampf(vx, -1.0f, 1.0f) * c->p.v_max;
	c->w_cmd = clampf(wz, -1.0f, 1.0f) * c->p.w_max;
}

static float dead_zone(float u, float dz, float eps)
{
	if (dz <= 0.0f)
		return u;
	return u + dz * clampf(u / eps, -1.0f, 1.0f);
}

void bb_ctrl_step(struct bb_ctrl *c, const struct bb_ctrl_in *in, float dt, struct bb_ctrl_out *out)
{
	const struct bb_params *p = &c->p;
	float u_bal, u_l, u_r, u_max, vbat, allowed_turn, du;
	float a;

	/* battery low-pass, protect against zero/garbage readings */
	vbat = in->vbat > 1.0f ? in->vbat : c->vbat_f;
	if (!c->started) {
		c->vbat_f = vbat;
		c->started = 1;
	}
	a = dt / (dt + 1.0f / (2.0f * BB_PI * p->vbat_lpf_hz));
	c->vbat_f += a * (vbat - c->vbat_f);
	u_max = p->u_max_frac * c->vbat_f;

	/* slow loops */
	if (++c->div_cnt >= p->speed_div) {
		float dts = dt * (float)c->div_cnt;
		float v_target = c->v_cmd * c->scale;
		float e_w;

		c->div_cnt = 0;
		c->v_lim = approach(c->v_lim, v_target, p->a_max * dts);
		c->w_lim = approach(c->w_lim, c->w_cmd, p->alpha_max * dts);
		c->theta_ref = bb_pid_step(&c->speed, c->v_lim - in->v_meas, 0.0f, dts);
		e_w = c->w_lim - in->yaw_rate;
		c->u_turn = bb_pid_step(&c->yaw, e_w, 0.0f, dts);
		c->u_turn = clampf(c->u_turn, -p->u_turn_max_frac * c->vbat_f, p->u_turn_max_frac * c->vbat_f);
	}

	/* inner loop: forward lean (positive error) drives the wheels forward */
	u_bal = bb_pid_step(&c->angle, in->pitch - c->theta_ref - c->theta_trim, in->pitch_rate, dt);
	u_bal += p->kv_ff * c->v_lim; /* back-EMF feedforward for the commanded speed */
	u_bal = clampf(u_bal, -u_max, u_max);

	/* headroom limiter: shrink the speed request as the balance demand approaches the supply */
	{
		float start = p->headroom_start * u_max;
		float span = u_max - start;
		float s = span > 0.0f ? (u_max - fabsf(u_bal)) / span : 1.0f;

		c->scale = clampf(s, 0.0f, 1.0f);
	}

	/* turn gets what is left after balance (balance has priority) */
	allowed_turn = u_max - fabsf(u_bal);
	if (allowed_turn < 0.0f)
		allowed_turn = 0.0f;
	{
		float ut = clampf(c->u_turn, -allowed_turn, allowed_turn);

		u_l = u_bal - ut;
		u_r = u_bal + ut;
	}
	u_l = clampf(dead_zone(u_l, p->u_dz, p->u_dz_eps), -u_max, u_max);
	u_r = clampf(dead_zone(u_r, p->u_dz, p->u_dz_eps), -u_max, u_max);

	/* slew limit per side */
	du = p->u_slew_v_per_s * dt;
	u_l = approach(c->u_l_prev, u_l, du);
	u_r = approach(c->u_r_prev, u_r, du);
	c->u_l_prev = u_l;
	c->u_r_prev = u_r;

	out->u_l = u_l;
	out->u_r = u_r;
	out->duty_l = clampf(u_l / c->vbat_f, -p->u_max_frac, p->u_max_frac);
	out->duty_r = clampf(u_r / c->vbat_f, -p->u_max_frac, p->u_max_frac);
	out->theta_ref = c->theta_ref;
	out->u_bal = u_bal;
	out->u_turn = c->u_turn;
	out->v_cmd_eff = c->v_lim;
}
