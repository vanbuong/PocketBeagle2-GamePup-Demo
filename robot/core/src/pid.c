/* SPDX-License-Identifier: GPL-2.0-only */
#include "balbot/pid.h"

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

void bb_pid_init(struct bb_pid *p, float kp, float ki, float kd, float out_min, float out_max, float i_limit)
{
	p->kp = kp;
	p->ki = ki;
	p->kd = kd;
	p->out_min = out_min;
	p->out_max = out_max;
	p->i_limit = i_limit;
	p->integ = 0.0f;
}

void bb_pid_reset(struct bb_pid *p)
{
	p->integ = 0.0f;
}

float bb_pid_step(struct bb_pid *p, float err, float dterm, float dt)
{
	float u = p->kp * err + p->integ + p->kd * dterm;
	int sat_hi = u > p->out_max && err > 0.0f;
	int sat_lo = u < p->out_min && err < 0.0f;

	if (!sat_hi && !sat_lo)
		p->integ = clampf(p->integ + p->ki * err * dt, -p->i_limit, p->i_limit);
	u = p->kp * err + p->integ + p->kd * dterm;
	return clampf(u, p->out_min, p->out_max);
}
