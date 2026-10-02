/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared-memory layouts between M4F and the PRUs (doc 01, 1.5). Include from the M4F, the
 * PRU firmware and host tests; the static asserts keep all sides honest. One writer per field. */
#ifndef BALBOT_MAILBOX_H
#define BALBOT_MAILBOX_H
#include <stddef.h>
#include <stdint.h>

#define BB_PRU_PWM_HZ 20000u

enum bb_pru1_mode { BB_PRU1_COAST = 0, BB_PRU1_DRIVE = 1, BB_PRU1_BRAKE = 2 };

/* PRU1 data RAM, written by the M4F */
struct bb_pru1_cmd {
	volatile uint32_t heartbeat;      /* 0x00 incremented every control tick */
	volatile int16_t duty_l;          /* 0x04 -1000..+1000 (per mille of the PWM period) */
	volatile int16_t duty_r;          /* 0x06 */
	volatile uint8_t mode;            /* 0x08 enum bb_pru1_mode */
	uint8_t _pad0[3];
	volatile uint16_t slew_limit;     /* 0x0C per mille per PWM period */
	uint8_t _pad1[2];
	volatile uint32_t pwm_period_ticks; /* 0x10 set once at init */
};

/* PRU1 status, written by PRU1 */
struct bb_pru1_status {
	volatile uint32_t applied_seq;    /* 0x00 heartbeat value last seen */
	volatile uint8_t failsafe_active; /* 0x04 */
	uint8_t _pad[3];
	volatile uint32_t periods;        /* 0x08 completed PWM periods */
};

/* PRU0 output, written by PRU0, read by the M4F with the seqlock protocol below */
struct bb_pru0_out {
	volatile int32_t count_l;         /* 0x00 */
	volatile int32_t count_r;         /* 0x04 */
	volatile uint32_t edge_ts_l;      /* 0x08 IEP timestamp of the last edge */
	volatile uint32_t edge_ts_r;      /* 0x0C */
	volatile uint32_t seq;            /* 0x10 odd while PRU0 is writing */
	volatile uint16_t illegal;        /* 0x14 invalid Gray-code transitions */
	uint8_t _pad[2];
};

_Static_assert(offsetof(struct bb_pru1_cmd, heartbeat) == 0x00, "pru1 cmd layout");
_Static_assert(offsetof(struct bb_pru1_cmd, duty_l) == 0x04, "pru1 cmd layout");
_Static_assert(offsetof(struct bb_pru1_cmd, duty_r) == 0x06, "pru1 cmd layout");
_Static_assert(offsetof(struct bb_pru1_cmd, mode) == 0x08, "pru1 cmd layout");
_Static_assert(offsetof(struct bb_pru1_cmd, slew_limit) == 0x0C, "pru1 cmd layout");
_Static_assert(offsetof(struct bb_pru1_cmd, pwm_period_ticks) == 0x10, "pru1 cmd layout");
_Static_assert(sizeof(struct bb_pru1_cmd) == 0x14, "pru1 cmd size");
_Static_assert(offsetof(struct bb_pru1_status, failsafe_active) == 0x04, "pru1 status layout");
_Static_assert(offsetof(struct bb_pru1_status, periods) == 0x08, "pru1 status layout");
_Static_assert(offsetof(struct bb_pru0_out, count_l) == 0x00, "pru0 layout");
_Static_assert(offsetof(struct bb_pru0_out, edge_ts_l) == 0x08, "pru0 layout");
_Static_assert(offsetof(struct bb_pru0_out, seq) == 0x10, "pru0 layout");
_Static_assert(offsetof(struct bb_pru0_out, illegal) == 0x14, "pru0 layout");
_Static_assert(sizeof(struct bb_pru0_out) == 0x18, "pru0 size");

/* Seqlock read for the M4F: retries until a consistent snapshot (seq even and unchanged). */
static inline int bb_pru0_snapshot(const struct bb_pru0_out *m, struct bb_pru0_out *dst, int max_tries)
{
	for (int i = 0; i < max_tries; i++) {
		uint32_t s1 = m->seq;

		if (s1 & 1u)
			continue;
		dst->count_l = m->count_l;
		dst->count_r = m->count_r;
		dst->edge_ts_l = m->edge_ts_l;
		dst->edge_ts_r = m->edge_ts_r;
		dst->illegal = m->illegal;
		if (m->seq == s1) {
			dst->seq = s1;
			return 0;
		}
	}
	return -1;
}

/* Quadrature decode step: 4x, returns +1/-1 for a valid transition, 0 for no change,
 * 2 for an illegal (skipped) transition. state = (A<<1)|B, previous and current. */
static inline int bb_qdec_step(uint8_t prev, uint8_t cur)
{
	/* index = prev<<2 | cur ; Gray sequence 00 -> 01 -> 11 -> 10 is +1 */
	static const int8_t t[16] = {0, 1, -1, 2, -1, 0, 2, 1, 1, 2, 0, -1, 2, -1, 1, 0};

	return t[((prev & 3u) << 2) | (cur & 3u)];
}

#endif
