/* SPDX-License-Identifier: GPL-2.0-only */
/* Cascaded balance controller (doc 05): inner angle PD(+I) at 500 Hz, outer speed PI and yaw PI
 * at 100 Hz, voltage normalisation, dead-zone compensation, slew limiting, headroom limiter.
 * Pure C, no allocation, float32. */
#ifndef BALBOT_BALANCE_H
#define BALBOT_BALANCE_H
#include <stdint.h>

#include "balbot/pid.h"

#ifdef __cplusplus
extern "C" {
#endif

struct bb_params {
	/* inner loop (volts per rad, volts*s per rad, volts per rad*s) */
	float kp_angle, ki_angle, kd_angle;
	/* outer speed loop: rad per (m/s), rad per m */
	float kp_speed, ki_speed;
	float theta_max;       /* rad, lean limit from the speed loop */
	float kv_ff;           /* V per (m/s): back-EMF feedforward, about Ke / wheel_radius */
	/* yaw loop: volts per rad/s, volts per rad */
	float kp_yaw, ki_yaw;
	float u_turn_max_frac; /* of vbat */
	/* limits */
	float v_max, w_max;    /* m/s, rad/s */
	float a_max, alpha_max; /* m/s^2, rad/s^2 */
	float u_max_frac;      /* of vbat, e.g. 0.95 */
	float u_slew_v_per_s;
	float u_dz, u_dz_eps;  /* dead-zone compensation volts / smoothing band */
	float headroom_start;  /* fraction of available volts where v_cmd starts to be reduced */
	/* timing */
	int speed_div;         /* run speed/yaw loops every N fast steps */
	float vbat_lpf_hz;
	float vbat_nom;
};

struct bb_ctrl_in {
	float pitch, pitch_rate;  /* rad, rad/s */
	float yaw_rate;           /* rad/s */
	float v_meas;             /* m/s, mean wheel speed */
	float vbat;               /* volts */
};

struct bb_ctrl_out {
	float u_l, u_r;       /* volts after limits, dead-zone and slew (what is applied) */
	float duty_l, duty_r; /* -1..1 */
	float theta_ref;      /* rad, from the speed loop */
	float u_bal, u_turn;  /* before mixing */
	float v_cmd_eff;      /* m/s after rate/headroom limiting */
};

struct bb_ctrl {
	struct bb_params p;
	struct bb_pid angle, speed, yaw;
	float v_cmd, w_cmd;        /* requested, already scaled to SI */
	float v_lim, w_lim;        /* rate limited */
	float theta_trim;          /* rad */
	float theta_ref;
	float u_turn;
	float scale;               /* headroom scale 0..1 */
	float vbat_f;
	float u_l_prev, u_r_prev;
	int div_cnt;
	int started;
};

void bb_params_default(struct bb_params *p);
void bb_ctrl_init(struct bb_ctrl *c, const struct bb_params *p);
void bb_ctrl_reset(struct bb_ctrl *c);
/* Normalised intent in [-1, 1] (clamped), scaled by v_max / w_max. */
void bb_ctrl_set_intent(struct bb_ctrl *c, float vx, float wz);
/* One fast step (dt = 1/500 s). Runs the slow loops every p.speed_div calls. */
void bb_ctrl_step(struct bb_ctrl *c, const struct bb_ctrl_in *in, float dt, struct bb_ctrl_out *out);

#ifdef __cplusplus
}
#endif
#endif
