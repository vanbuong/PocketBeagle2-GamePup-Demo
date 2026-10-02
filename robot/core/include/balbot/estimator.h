/* SPDX-License-Identifier: GPL-2.0-only */
/* Pitch estimators (doc 04, 4.8 A and B).
 * Convention (include/balbot/conventions.h): body x forward, y left, z up; pitch is the
 * rotation about +y, positive = leaning FORWARD. At rest a_x = -sin(pitch) g, a_z = cos(pitch) g,
 * so pitch_acc = atan2(-a_x, a_z). The gyro rate about +y is the pitch rate. */
#ifndef BALBOT_ESTIMATOR_H
#define BALBOT_ESTIMATOR_H

#ifdef __cplusplus
extern "C" {
#endif

/* accelerometer pitch, radians */
float bb_accel_pitch(float ax_g, float az_g);
/* weight 0..1: 1 when |a| = 1 g, linearly 0 at |dev| >= gate_g */
float bb_accel_weight(float ax_g, float ay_g, float az_g, float gate_g);

/* ---- A. complementary filter ---- */
struct bb_cf {
	float tau;      /* s */
	float gate_g;   /* accel validity gate (g) */
	float pitch;    /* rad */
	float rate;     /* rad/s, = gyro - bias */
	float bias;     /* rad/s, subtracted from the gyro before use */
	int started;
};
void bb_cf_init(struct bb_cf *f, float tau_s, float gate_g);
/* gyro_y: rad/s about +y (bias NOT yet removed if f->bias != 0) */
void bb_cf_update(struct bb_cf *f, float gyro_y, float ax_g, float ay_g, float az_g, float dt);

/* ---- B. two-state Kalman filter (pitch, gyro bias) ---- */
struct bb_kf {
	float pitch, bias, rate;
	float P[2][2];
	float q_pitch, q_bias;  /* process noise densities */
	float r_acc;            /* accel pitch variance, rad^2 */
	float r_inflate;        /* multiplier when accel invalid (weight < 1) */
	float gate_g;
	int started;
};
void bb_kf_init(struct bb_kf *f, float q_pitch, float q_bias, float r_acc, float gate_g);
void bb_kf_update(struct bb_kf *f, float gyro_y, float ax_g, float ay_g, float az_g, float dt);

#ifdef __cplusplus
}
#endif
#endif
