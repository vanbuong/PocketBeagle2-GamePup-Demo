/* SPDX-License-Identifier: GPL-2.0-only */
#include "balbot/speed.h"

#include <string.h>

void bb_speed_win_init(struct bb_speed_win *s, float m_per_count, float alpha)
{
	memset(s, 0, sizeof(*s));
	s->m_per_count = m_per_count;
	s->alpha = alpha;
}

float bb_speed_win_update(struct bb_speed_win *s, int32_t count, float dt)
{
	if (s->started && dt > 0.0f) {
		/* unsigned subtraction keeps the 32-bit wrap harmless */
		int32_t d = (int32_t)((uint32_t)count - (uint32_t)s->last_count);
		float raw = (float)d * s->m_per_count / dt;

		s->v += s->alpha * (raw - s->v);
	}
	s->started = 1;
	s->last_count = count;
	return s->v;
}

void bb_speed_mt_init(struct bb_speed_mt *s, float m_per_count, float tick_hz, uint32_t timeout_ticks)
{
	memset(s, 0, sizeof(*s));
	s->m_per_count = m_per_count;
	s->tick_hz = tick_hz;
	s->timeout_ticks = timeout_ticks;
}

float bb_speed_mt_update(struct bb_speed_mt *s, int32_t count, uint32_t last_edge_ts, uint32_t now)
{
	if (!s->started) {
		s->started = 1;
		s->c0 = count;
		s->t0 = last_edge_ts;
		return 0.0f;
	}
	if (count != s->c0) {
		uint32_t dt_ticks = last_edge_ts - s->t0;
		int32_t dc = (int32_t)((uint32_t)count - (uint32_t)s->c0);

		if (dt_ticks != 0)
			s->v = (float)dc * s->m_per_count * s->tick_hz / (float)dt_ticks;
		s->c0 = count;
		s->t0 = last_edge_ts;
	} else if ((uint32_t)(now - s->t0) > s->timeout_ticks) {
		s->v = 0.0f;
	}
	return s->v;
}
