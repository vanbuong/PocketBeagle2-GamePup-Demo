/* SPDX-License-Identifier: GPL-2.0-only */
/* Wheel speed from quadrature counts (doc 08, 8.1). With 1320 counts/rev and a 65 mm wheel
 * one count is 0.155 mm: use a 10 ms window at 100 Hz, and the M/T (edge timestamp) method
 * below ~0.15 m/s. */
#ifndef BALBOT_SPEED_H
#define BALBOT_SPEED_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- fixed window ---- */
struct bb_speed_win {
	float m_per_count;
	int32_t last_count;
	int started;
	float v;       /* low-passed, m/s */
	float alpha;   /* low-pass coefficient per update, 1 = no filter */
};
void bb_speed_win_init(struct bb_speed_win *s, float m_per_count, float alpha);
/* count: signed running count; dt: seconds since the previous call */
float bb_speed_win_update(struct bb_speed_win *s, int32_t count, float dt);

/* ---- M/T: counts between the first and last edge timestamps ---- */
struct bb_speed_mt {
	float m_per_count;
	float tick_hz;       /* edge timestamp clock */
	uint32_t timeout_ticks; /* no new edge for this long => speed 0 */
	int32_t c0;
	uint32_t t0;
	int started;
	float v;
};
void bb_speed_mt_init(struct bb_speed_mt *s, float m_per_count, float tick_hz, uint32_t timeout_ticks);
/* count/last_edge_ts: PRU0 snapshot; now: current timestamp (same clock) */
float bb_speed_mt_update(struct bb_speed_mt *s, int32_t count, uint32_t last_edge_ts, uint32_t now);

#ifdef __cplusplus
}
#endif
#endif
