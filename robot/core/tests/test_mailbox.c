/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "balbot/mailbox.h"
#include "tst.h"

static void layouts_are_checked_at_compile_time(void)
{
	/* the _Static_asserts in the header are the real test; this keeps the file referenced */
	CHECK_EQ(sizeof(struct bb_pru1_cmd), 0x14);
	CHECK_EQ(sizeof(struct bb_pru0_out), 0x18);
	CHECK_EQ(BB_PRU_PWM_HZ, 20000);
}

static void qdec_forward_reverse_and_illegal(void)
{
	static const uint8_t fwd[] = {0, 1, 3, 2, 0, 1, 3, 2, 0}; /* A<<1|B, Gray sequence */
	int pos = 0, illegal = 0;

	for (unsigned i = 1; i < sizeof fwd; i++) {
		int s = bb_qdec_step(fwd[i - 1], fwd[i]);

		CHECK_EQ(s, 1);
		pos += s;
	}
	CHECK_EQ(pos, 8); /* two full cycles = 8 counts at 4x */
	for (int i = (int)sizeof fwd - 1; i > 0; i--) {
		int s = bb_qdec_step(fwd[i], fwd[i - 1]);

		CHECK_EQ(s, -1);
		pos += s;
	}
	CHECK_EQ(pos, 0);
	/* all 16 transitions: exactly 4 forward, 4 reverse, 4 no-change, 4 illegal */
	int nf = 0, nr = 0, n0 = 0, ni = 0;

	for (int p = 0; p < 4; p++)
		for (int c = 0; c < 4; c++) {
			int s = bb_qdec_step((uint8_t)p, (uint8_t)c);

			nf += s == 1;
			nr += s == -1;
			n0 += s == 0;
			ni += s == 2;
		}
	CHECK_EQ(nf, 4);
	CHECK_EQ(nr, 4);
	CHECK_EQ(n0, 4);
	CHECK_EQ(ni, 4);
	(void)illegal;
}

static void qdec_noise_chatter_does_not_drift(void)
{
	/* contact bounce between two adjacent states nets to zero */
	int pos = 0;
	uint8_t prev = 1;

	for (int i = 0; i < 1000; i++) {
		uint8_t cur = (i & 1) ? 1 : 3;

		pos += bb_qdec_step(prev, cur);
		prev = cur;
	}
	CHECK(pos >= -1 && pos <= 1);
}

static void seqlock_snapshot(void)
{
	struct bb_pru0_out m, snap;

	memset(&m, 0, sizeof m);
	m.count_l = -5;
	m.count_r = 7;
	m.edge_ts_l = 100;
	m.edge_ts_r = 200;
	m.illegal = 3;
	m.seq = 4;
	memset(&snap, 0, sizeof snap);
	CHECK_EQ(bb_pru0_snapshot(&m, &snap, 3), 0);
	CHECK_EQ(snap.count_l, -5);
	CHECK_EQ(snap.count_r, 7);
	CHECK_EQ(snap.illegal, 3);
	m.seq = 5; /* PRU0 mid-update: odd sequence, reader must give up after the retry limit */
	CHECK_EQ(bb_pru0_snapshot(&m, &snap, 3), -1);
}

int main(void)
{
	RUN(layouts_are_checked_at_compile_time);
	RUN(qdec_forward_reverse_and_illegal);
	RUN(qdec_noise_chatter_does_not_drift);
	RUN(seqlock_snapshot);
	TEST_MAIN_END();
}
