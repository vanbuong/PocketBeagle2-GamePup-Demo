/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BALBOT_PID_H
#define BALBOT_PID_H

#ifdef __cplusplus
extern "C" {
#endif

/* out = kp*err + I + kd*dterm, where dterm is supplied by the caller (derivative on
 * MEASUREMENT: pass the measured rate with the sign that matches the loop, so a setpoint step
 * produces no derivative kick). I integrates ki*err with conditional anti-windup and a clamp. */
struct bb_pid {
	float kp, ki, kd;
	float out_min, out_max;
	float i_limit;  /* |I| <= i_limit */
	float integ;
};

void bb_pid_init(struct bb_pid *p, float kp, float ki, float kd, float out_min, float out_max, float i_limit);
void bb_pid_reset(struct bb_pid *p);
float bb_pid_step(struct bb_pid *p, float err, float dterm, float dt);

#ifdef __cplusplus
}
#endif
#endif
