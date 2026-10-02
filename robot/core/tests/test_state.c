/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "balbot/imu.h"
#include "balbot/state.h"
#include "tst.h"

#define DEG (BB_PI / 180.0f)
#define DT 0.002f

static struct bb_sm_in good(void)
{
	struct bb_sm_in i;

	memset(&i, 0, sizeof i);
	i.vbat = 11.1f;
	i.imu_ok = true;
	i.link_ok = true;
	return i;
}

static void run(struct bb_sm *s, struct bb_sm_in *in, float seconds)
{
	int n = (int)(seconds / DT + 0.5f);

	for (int k = 0; k < n; k++)
		bb_sm_step(s, in, DT);
}

static void to_standby(struct bb_sm *s, struct bb_sm_in *in)
{
	struct bb_sm_cfg c;

	bb_sm_cfg_default(&c);
	bb_sm_init(s, &c);
	*in = good();
	bb_sm_step(s, in, DT);
	in->cal_done = true;
	bb_sm_step(s, in, DT);
	in->cal_done = false;
}

static void to_balancing(struct bb_sm *s, struct bb_sm_in *in)
{
	to_standby(s, in);
	CHECK(bb_sm_command(s, BB_SMCMD_ARM, in));
	run(s, in, 1.1f);
}

static void normal_start_up_path(void)
{
	struct bb_sm s;
	struct bb_sm_in in;
	struct bb_sm_cfg c;

	bb_sm_cfg_default(&c);
	bb_sm_init(&s, &c);
	in = good();
	CHECK_EQ(s.state, BB_ST_BOOT);
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_CALIBRATING);
	run(&s, &in, 1.0f);
	CHECK_EQ(s.state, BB_ST_CALIBRATING); /* waits for the calibration */
	in.cal_done = true;
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_STANDBY);
	CHECK(!bb_sm_motors_active(&s));
	in.cal_done = false;
	CHECK(bb_sm_command(&s, BB_SMCMD_ARM, &in));
	run(&s, &in, 0.5f);
	CHECK_EQ(s.state, BB_ST_STANDBY); /* needs 1 s upright */
	run(&s, &in, 0.6f);
	CHECK_EQ(s.state, BB_ST_BALANCING);
	CHECK(bb_sm_motors_active(&s));
	CHECK(strcmp(bb_state_name(s.state), "BALANCING") == 0);
}

static void arm_preconditions(void)
{
	struct bb_sm s;
	struct bb_sm_in in;

	to_standby(&s, &in);
	in.vbat = 9.0f;
	CHECK(!bb_sm_command(&s, BB_SMCMD_ARM, &in)); /* low battery */
	in.vbat = 11.1f;
	in.imu_ok = true;
	in.estop_active = false;
	/* tilted: request accepted but never completes, and times out */
	CHECK(bb_sm_command(&s, BB_SMCMD_ARM, &in));
	in.pitch = 10.0f * DEG;
	run(&s, &in, 3.0f);
	CHECK_EQ(s.state, BB_ST_STANDBY);
	/* leaves the window mid-hold: timer restarts */
	in.pitch = 0.0f;
	run(&s, &in, 0.8f);
	in.pitch = 6.0f * DEG;
	run(&s, &in, 0.1f);
	in.pitch = 0.0f;
	run(&s, &in, 0.8f);
	CHECK_EQ(s.state, BB_ST_STANDBY);
	run(&s, &in, 0.3f);
	CHECK_EQ(s.state, BB_ST_BALANCING);
	/* pending request expires after 10 s */
	to_standby(&s, &in);
	CHECK(bb_sm_command(&s, BB_SMCMD_ARM, &in));
	in.pitch = 20.0f * DEG;
	run(&s, &in, 10.5f);
	in.pitch = 0.0f;
	run(&s, &in, 2.0f);
	CHECK_EQ(s.state, BB_ST_STANDBY);
	/* not calibrated yet: ARM refused in CALIBRATING */
	struct bb_sm_cfg c;

	bb_sm_cfg_default(&c);
	bb_sm_init(&s, &c);
	in = good();
	bb_sm_step(&s, &in, DT);
	CHECK(!bb_sm_command(&s, BB_SMCMD_ARM, &in));
	CHECK(s.refused >= 1);
}

static void tip_over_gives_fallen_with_motors_off(void)
{
	struct bb_sm s;
	struct bb_sm_in in;

	to_balancing(&s, &in);
	in.pitch = 34.0f * DEG;
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_BALANCING);
	in.pitch = 36.0f * DEG;
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_FALLEN); /* one sample is enough */
	CHECK(!bb_sm_motors_active(&s));
	in.pitch = -36.0f * DEG;
	to_balancing(&s, &in);
	in.pitch = -36.0f * DEG;
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_FALLEN);
}

static void fallen_recovery_needs_upright_and_disarm(void)
{
	struct bb_sm s;
	struct bb_sm_in in;

	to_balancing(&s, &in);
	in.pitch = 60.0f * DEG;
	bb_sm_step(&s, &in, DT);
	in.pitch = 0.0f;
	run(&s, &in, 3.0f);
	CHECK_EQ(s.state, BB_ST_FALLEN); /* upright long enough but no DISARM seen */
	bb_sm_command(&s, BB_SMCMD_DISARM, &in);
	CHECK_EQ(s.state, BB_ST_FALLEN); /* the command alone does not move the machine */
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_STANDBY); /* upright >= 2 s and DISARM seen */
	/* DISARM first, then not yet upright long enough */
	to_balancing(&s, &in);
	in.pitch = 60.0f * DEG;
	bb_sm_step(&s, &in, DT);
	bb_sm_command(&s, BB_SMCMD_DISARM, &in);
	in.pitch = 0.0f;
	run(&s, &in, 1.0f);
	CHECK_EQ(s.state, BB_ST_FALLEN);
	in.pitch = 20.0f * DEG; /* leaves the window: timer restarts */
	run(&s, &in, 0.5f);
	in.pitch = 0.0f;
	run(&s, &in, 1.9f);
	CHECK_EQ(s.state, BB_ST_FALLEN);
	run(&s, &in, 0.2f);
	CHECK_EQ(s.state, BB_ST_STANDBY);
}

static void command_and_link_timeouts(void)
{
	struct bb_sm s;
	struct bb_sm_in in;

	to_balancing(&s, &in);
	in.link_ok = false;
	run(&s, &in, 9.5f);
	CHECK_EQ(s.state, BB_ST_BALANCING); /* still balancing, speed command is zeroed upstream */
	run(&s, &in, 1.0f);
	CHECK_EQ(s.state, BB_ST_LAYING_DOWN);
	CHECK(bb_sm_motors_active(&s));
	in.pitch = 40.0f * DEG; /* > tip angle must NOT trigger FALLEN while laying down */
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_LAYING_DOWN);
	in.pitch = 72.0f * DEG;
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_STANDBY);
	/* link recovering resets the timer */
	to_balancing(&s, &in);
	in.link_ok = false;
	run(&s, &in, 9.0f);
	in.link_ok = true;
	run(&s, &in, 0.1f);
	in.link_ok = false;
	run(&s, &in, 9.0f);
	CHECK_EQ(s.state, BB_ST_BALANCING);
}

static void low_battery_lays_down_then_blocks_arm(void)
{
	struct bb_sm s;
	struct bb_sm_in in;

	to_balancing(&s, &in);
	in.vbat = 9.8f;
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_LAYING_DOWN);
	in.vbat = 11.1f;
	in.pitch = 80.0f * DEG;
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_STANDBY);
	in.vbat = 9.8f;
	CHECK(!bb_sm_command(&s, BB_SMCMD_ARM, &in));
	/* below the cut-off the motors stop at once */
	to_balancing(&s, &in);
	in.vbat = 9.5f;
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_STANDBY);
}

static void laydown_times_out(void)
{
	struct bb_sm s;
	struct bb_sm_in in;

	to_balancing(&s, &in);
	in.vbat = 9.8f;
	bb_sm_step(&s, &in, DT);
	in.vbat = 11.1f;
	run(&s, &in, 5.1f);
	CHECK_EQ(s.state, BB_ST_STANDBY);
}

static void faults_latch_and_reset_rules(void)
{
	struct bb_sm s;
	struct bb_sm_in in;

	to_balancing(&s, &in);
	in.imu_ok = false;
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_FAULT);
	CHECK(s.faults & BB_FAULT_IMU);
	CHECK(!bb_sm_motors_active(&s));
	in.imu_ok = true;
	run(&s, &in, 1.0f);
	CHECK_EQ(s.state, BB_ST_FAULT); /* latched, does not clear by itself */
	CHECK(!bb_sm_command(&s, BB_SMCMD_ARM, &in));
	/* reset refused while a new fault is still being raised */
	in.fault_bits = BB_FAULT_OVERRUN;
	CHECK(!bb_sm_command(&s, BB_SMCMD_RESET, &in));
	in.fault_bits = 0;
	CHECK(bb_sm_command(&s, BB_SMCMD_RESET, &in));
	CHECK_EQ(s.state, BB_ST_BOOT);
	CHECK_EQ(s.faults, 0);
	/* raised fault bits in any state */
	to_standby(&s, &in);
	in.fault_bits = BB_FAULT_STALL;
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_FAULT);
	CHECK(s.faults & BB_FAULT_STALL);
	/* calibration failure */
	struct bb_sm_cfg c;

	bb_sm_cfg_default(&c);
	bb_sm_init(&s, &c);
	in = good();
	bb_sm_step(&s, &in, DT);
	in.cal_failed = true;
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_FAULT);
	CHECK(s.faults & BB_FAULT_CAL);
}

static void estop_is_accepted_everywhere_and_latches(void)
{
	for (int st = 0; st < BB_ST_COUNT; st++) {
		struct bb_sm s;
		struct bb_sm_in in;

		to_standby(&s, &in);
		switch (st) {
		case BB_ST_BOOT: {
			struct bb_sm_cfg c;

			bb_sm_cfg_default(&c);
			bb_sm_init(&s, &c);
			break;
		}
		case BB_ST_CALIBRATING:
			bb_sm_init(&s, &s.cfg);
			bb_sm_step(&s, &in, DT);
			break;
		case BB_ST_STANDBY:
			break;
		case BB_ST_BALANCING:
			bb_sm_command(&s, BB_SMCMD_ARM, &in);
			run(&s, &in, 1.1f);
			break;
		case BB_ST_LAYING_DOWN:
			bb_sm_command(&s, BB_SMCMD_ARM, &in);
			run(&s, &in, 1.1f);
			in.vbat = 9.8f;
			bb_sm_step(&s, &in, DT);
			in.vbat = 11.1f;
			break;
		case BB_ST_FALLEN:
			bb_sm_command(&s, BB_SMCMD_ARM, &in);
			run(&s, &in, 1.1f);
			in.pitch = 1.2f;
			bb_sm_step(&s, &in, DT);
			in.pitch = 0.0f;
			break;
		case BB_ST_FAULT:
			in.fault_bits = BB_FAULT_OVERRUN;
			bb_sm_step(&s, &in, DT);
			in.fault_bits = 0;
			break;
		}
		CHECK_EQ(s.state, st);
		CHECK(bb_sm_command(&s, BB_SMCMD_ESTOP, &in));
		CHECK_EQ(s.state, BB_ST_FAULT);
		CHECK(s.faults & BB_FAULT_ESTOP);
		CHECK(!bb_sm_motors_active(&s));
	}
	/* hardware estop line: forces FAULT every tick while active, reset refused until released */
	struct bb_sm s;
	struct bb_sm_in in;

	to_balancing(&s, &in);
	in.estop_active = true;
	bb_sm_step(&s, &in, DT);
	CHECK_EQ(s.state, BB_ST_FAULT);
	CHECK(!bb_sm_command(&s, BB_SMCMD_RESET, &in));
	in.estop_active = false;
	CHECK(bb_sm_command(&s, BB_SMCMD_RESET, &in));
}

static void disarm_from_every_state_is_safe(void)
{
	struct bb_sm s;
	struct bb_sm_in in;

	to_balancing(&s, &in);
	CHECK(bb_sm_command(&s, BB_SMCMD_DISARM, &in));
	CHECK_EQ(s.state, BB_ST_STANDBY);
	CHECK(bb_sm_command(&s, BB_SMCMD_DISARM, &in)); /* idempotent */
	CHECK_EQ(s.state, BB_ST_STANDBY);
	bb_sm_init(&s, &s.cfg);
	CHECK(bb_sm_command(&s, BB_SMCMD_DISARM, &in)); /* BOOT */
	CHECK_EQ(s.state, BB_ST_BOOT);
}

static void fuzz_motors_only_active_in_allowed_states_with_good_history(void)
{
	/* random event sequences: the motors can only be active if the machine reached BALANCING
	 * through a legal arm (i.e. not from FAULT/FALLEN/BOOT/CALIBRATING directly) */
	for (int seed = 0; seed < 200; seed++) {
		struct bb_sm s;
		struct bb_sm_in in;
		struct bb_sm_cfg c;
		enum bb_state prev;

		bb_sm_cfg_default(&c);
		bb_sm_init(&s, &c);
		in = good();
		for (int k = 0; k < 3000; k++) {
			prev = s.state;
			if (tst_rand() % 50 == 0)
				in.pitch = (tst_randf()) * 90.0f * DEG;
			if (tst_rand() % 200 == 0)
				in.vbat = 9.0f + 3.6f * (tst_randf() * 0.5f + 0.5f);
			in.imu_ok = tst_rand() % 500 != 0;
			in.link_ok = tst_rand() % 20 != 0;
			in.cal_done = tst_rand() % 100 == 0;
			in.cal_failed = tst_rand() % 1000 == 0;
			in.estop_active = tst_rand() % 800 == 0;
			in.fault_bits = tst_rand() % 1000 == 0 ? BB_FAULT_OVERRUN : 0;
			switch (tst_rand() % 40) {
			case 0: bb_sm_command(&s, BB_SMCMD_ARM, &in); break;
			case 1: bb_sm_command(&s, BB_SMCMD_DISARM, &in); break;
			case 2: bb_sm_command(&s, BB_SMCMD_RESET, &in); break;
			case 3: if (tst_rand() % 10 == 0) bb_sm_command(&s, BB_SMCMD_ESTOP, &in); break;
			default: break;
			}
			bb_sm_step(&s, &in, DT * 10.0f);
			CHECK(s.state < BB_ST_COUNT);
			if (bb_sm_motors_active(&s)) {
				CHECK(prev == BB_ST_STANDBY || prev == BB_ST_BALANCING || prev == BB_ST_LAYING_DOWN);
				CHECK(s.faults == 0);
			}
			if (s.state == BB_ST_BALANCING && prev != BB_ST_BALANCING)
				CHECK(prev == BB_ST_STANDBY);
		}
	}
}

int main(void)
{
	RUN(normal_start_up_path);
	RUN(arm_preconditions);
	RUN(tip_over_gives_fallen_with_motors_off);
	RUN(fallen_recovery_needs_upright_and_disarm);
	RUN(command_and_link_timeouts);
	RUN(low_battery_lays_down_then_blocks_arm);
	RUN(laydown_times_out);
	RUN(faults_latch_and_reset_rules);
	RUN(estop_is_accepted_everywhere_and_latches);
	RUN(disarm_from_every_state_is_safe);
	RUN(fuzz_motors_only_active_in_allowed_states_with_good_history);
	TEST_MAIN_END();
}
