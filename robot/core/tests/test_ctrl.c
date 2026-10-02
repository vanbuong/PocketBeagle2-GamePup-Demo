/* SPDX-License-Identifier: GPL-2.0-only */
#include "balbot/balance.h"
#include "balbot/imu.h"
#include "tst.h"

#define DT 0.002f
#define DEG (BB_PI / 180.0f)

static void setup(struct bb_ctrl *c, struct bb_params *p)
{
	bb_params_default(p);
	p->u_dz = 0.0f;
	bb_ctrl_init(c, p);
}

static struct bb_ctrl_in in0(void)
{
	struct bb_ctrl_in i = {0, 0, 0, 0, 11.1f};

	return i;
}

static void run_steps(struct bb_ctrl *c, struct bb_ctrl_in *in, int n, struct bb_ctrl_out *out)
{
	for (int i = 0; i < n; i++)
		bb_ctrl_step(c, in, DT, out);
}

static void sign_conventions(void)
{
	struct bb_ctrl c;
	struct bb_params p;
	struct bb_ctrl_out o;
	struct bb_ctrl_in i = in0();

	/* leaning forward -> positive volts on both wheels */
	setup(&c, &p);
	i.pitch = 3.0f * DEG;
	run_steps(&c, &i, 50, &o);
	CHECK(o.u_l > 0.0f && o.u_r > 0.0f);
	/* leaning backward -> negative */
	setup(&c, &p);
	i.pitch = -3.0f * DEG;
	run_steps(&c, &i, 50, &o);
	CHECK(o.u_l < 0.0f && o.u_r < 0.0f);
	/* pitching forward fast at zero angle -> positive (damping) */
	setup(&c, &p);
	i.pitch = 0.0f;
	i.pitch_rate = 0.5f;
	run_steps(&c, &i, 50, &o);
	CHECK(o.u_l > 0.0f);
}

static void speed_request_gives_forward_lean_reference_and_initial_reverse_push(void)
{
	struct bb_ctrl c;
	struct bb_params p;
	struct bb_ctrl_out o;
	struct bb_ctrl_in i = in0();

	setup(&c, &p);
	bb_ctrl_set_intent(&c, 1.0f, 0.0f); /* full forward */
	run_steps(&c, &i, 100, &o);
	CHECK(o.theta_ref > 0.0f);          /* lean forward wanted ... */
	CHECK(o.u_l < 0.0f);                /* ... so the wheels first drive BACKWARD (pitch still 0) */
	CHECK(o.theta_ref <= p.theta_max + 1e-6f);
}

static void yaw_command_turns_left_with_right_wheel_faster(void)
{
	struct bb_ctrl c;
	struct bb_params p;
	struct bb_ctrl_out o;
	struct bb_ctrl_in i = in0();

	setup(&c, &p);
	bb_ctrl_set_intent(&c, 0.0f, 1.0f); /* positive yaw = left */
	run_steps(&c, &i, 200, &o);
	CHECK(o.u_r > o.u_l);
	CHECK_NEAR(o.u_r + o.u_l, 0.0, 1e-3); /* pure rotation about the centre */
}

static void voltage_normalisation(void)
{
	double vb[] = {9.0, 10.0, 11.1, 12.6};

	for (unsigned k = 0; k < 4; k++) {
		struct bb_ctrl c;
		struct bb_params p;
		struct bb_ctrl_out o;
		struct bb_ctrl_in i = in0();

		setup(&c, &p);
		i.vbat = (float)vb[k];
		i.pitch = 1.0f * DEG;
		run_steps(&c, &i, 2000, &o); /* long enough for the battery low-pass and slew to settle */
		CHECK_NEAR(o.duty_l, o.u_l / vb[k], 1e-3);
	}
	/* duty never exceeds the limit */
	struct bb_ctrl c;
	struct bb_params p;
	struct bb_ctrl_out o;
	struct bb_ctrl_in i = in0();

	setup(&c, &p);
	i.pitch = 30.0f * DEG;
	run_steps(&c, &i, 2000, &o);
	CHECK(o.duty_l <= p.u_max_frac + 1e-6f);
	CHECK_NEAR(o.duty_l, p.u_max_frac, 1e-3);
	i.pitch = -30.0f * DEG;
	run_steps(&c, &i, 2000, &o);
	CHECK_NEAR(o.duty_l, -p.u_max_frac, 1e-3);
}

static void bad_battery_reading_is_ignored(void)
{
	struct bb_ctrl c;
	struct bb_params p;
	struct bb_ctrl_out o;
	struct bb_ctrl_in i = in0();

	setup(&c, &p);
	i.pitch = 1.0f * DEG;
	run_steps(&c, &i, 1000, &o);
	i.vbat = 0.0f; /* sensor glitch */
	run_steps(&c, &i, 100, &o);
	CHECK(o.duty_l == o.duty_l); /* not NaN */
	CHECK(o.duty_l < p.u_max_frac + 1e-6f && o.duty_l > 0.0f);
}

static void dead_zone_compensation_is_continuous_and_signed(void)
{
	struct bb_ctrl c;
	struct bb_params p;
	struct bb_ctrl_out o;
	struct bb_ctrl_in i = in0();
	float prev = 0.0f, maxjump = 0.0f;

	bb_params_default(&p);
	p.u_dz = 1.0f;
	p.u_slew_v_per_s = 1e6f;
	bb_ctrl_init(&c, &p);
	/* sweep pitch through zero slowly: output must be continuous (no chatter jump bigger than slope allows) */
	for (int k = -200; k <= 200; k++) {
		i.pitch = (float)k * 0.00005f;
		bb_ctrl_step(&c, &i, DT, &o);
		if (k > -200 && fabsf(o.u_l - prev) > maxjump)
			maxjump = fabsf(o.u_l - prev);
		prev = o.u_l;
	}
	CHECK(maxjump < 0.5f * p.u_dz); /* the dead-zone term is smoothed over u_dz_eps */
	/* sign: beyond the band the compensation adds in the direction of the command */
	setup(&c, &p);
	bb_params_default(&p);
	p.u_dz = 1.0f;
	bb_ctrl_init(&c, &p);
	i.pitch = 1.0f * DEG;
	run_steps(&c, &i, 500, &o);
	float with = o.u_l;

	bb_params_default(&p);
	p.u_dz = 0.0f;
	bb_ctrl_init(&c, &p);
	run_steps(&c, &i, 500, &o);
	CHECK_NEAR(with - o.u_l, 1.0, 1e-3);
}

static void limiters_hold_for_random_commands(void)
{
	struct bb_ctrl c;
	struct bb_params p;
	struct bb_ctrl_out o;
	struct bb_ctrl_in i = in0();
	float prev_v = 0.0f, prev_ul = 0.0f, prev_ur = 0.0f;

	setup(&c, &p);
	for (int k = 0; k < 100000; k++) {
		if (k % 200 == 0)
			bb_ctrl_set_intent(&c, 3.0f * tst_randf(), 3.0f * tst_randf()); /* out-of-range intents too */
		i.pitch = 0.05f * tst_randf();
		i.pitch_rate = 0.5f * tst_randf();
		i.yaw_rate = 2.0f * tst_randf();
		i.v_meas = 0.5f * tst_randf();
		i.vbat = 9.0f + 3.6f * (tst_randf() * 0.5f + 0.5f);
		bb_ctrl_step(&c, &i, DT, &o);
		CHECK(fabsf(o.theta_ref) <= p.theta_max + 1e-5f);
		CHECK(fabsf(o.v_cmd_eff) <= p.v_max + 1e-5f);
		CHECK(fabsf(c.w_lim) <= p.w_max + 1e-5f);
		CHECK(fabsf(o.duty_l) <= p.u_max_frac + 1e-5f && fabsf(o.duty_r) <= p.u_max_frac + 1e-5f);
		CHECK(fabsf(o.u_l - prev_ul) <= p.u_slew_v_per_s * DT + 1e-4f);
		CHECK(fabsf(o.u_r - prev_ur) <= p.u_slew_v_per_s * DT + 1e-4f);
		/* acceleration limit is applied once per slow step */
		if (k % p.speed_div == 0)
			CHECK(fabsf(o.v_cmd_eff - prev_v) <= p.a_max * DT * (float)p.speed_div + 1e-5f || k == 0);
		if (k % p.speed_div == p.speed_div - 1)
			prev_v = o.v_cmd_eff;
		prev_ul = o.u_l;
		prev_ur = o.u_r;
	}
}

static void turn_is_reduced_before_balance_when_voltage_is_short(void)
{
	struct bb_ctrl c;
	struct bb_params p;
	struct bb_ctrl_out o;
	struct bb_ctrl_in i = in0();

	setup(&c, &p);
	i.vbat = 9.0f;
	i.pitch = 0.0f;
	i.pitch_rate = 0.0f;
	run_steps(&c, &i, 500, &o);
	/* Big forward lean demand uses almost the whole supply; a hard turn must not take it away */
	bb_ctrl_set_intent(&c, 0.0f, 1.0f);
	i.pitch = 12.0f * DEG;
	i.yaw_rate = -5.0f; /* large yaw error asks for a lot of turn */
	run_steps(&c, &i, 3000, &o);
	float umax = p.u_max_frac * 9.0f;

	CHECK(o.u_l <= umax + 1e-3f && o.u_r <= umax + 1e-3f);
	CHECK(fabsf((o.u_l + o.u_r) * 0.5f - o.u_bal) < 0.1f + 1e-3f * umax || fabsf(o.u_bal) >= umax - 0.01f);
	CHECK(o.u_bal > 0.0f); /* balance command still points the right way */
}

static void headroom_limiter_cuts_speed_request(void)
{
	struct bb_ctrl c;
	struct bb_params p;
	struct bb_ctrl_out o;
	struct bb_ctrl_in i = in0();

	setup(&c, &p);
	bb_ctrl_set_intent(&c, 1.0f, 0.0f);
	/* balance demand nearly saturated */
	i.pitch = 12.0f * DEG;
	run_steps(&c, &i, 3000, &o);
	CHECK(c.scale < 0.05f);
	CHECK(o.v_cmd_eff < 0.05f);
	/* demand relaxes (request dropped, lean integrator cleared): headroom returns to full */
	bb_ctrl_set_intent(&c, 0.0f, 0.0f);
	bb_pid_reset(&c.speed);
	i.pitch = 0.0f;
	run_steps(&c, &i, 3000, &o);
	CHECK(fabsf(o.u_bal) < 1.0f);
	CHECK_NEAR(c.scale, 1.0, 1e-3);
}

static void reset_clears_state(void)
{
	struct bb_ctrl c;
	struct bb_params p;
	struct bb_ctrl_out o;
	struct bb_ctrl_in i = in0();

	setup(&c, &p);
	bb_ctrl_set_intent(&c, 1.0f, 1.0f);
	i.pitch = 0.1f;
	run_steps(&c, &i, 500, &o);
	bb_ctrl_reset(&c);
	i.pitch = 0.0f;
	run_steps(&c, &i, 5, &o);
	CHECK_NEAR(o.u_l, 0.0, 1e-4);
	CHECK_NEAR(o.u_r, 0.0, 1e-4);
	CHECK_NEAR(c.v_cmd, 0.0, 1e-9);
}

static void deterministic(void)
{
	struct bb_ctrl a, b;
	struct bb_params p;
	struct bb_ctrl_out oa, ob;
	struct bb_ctrl_in i = in0();

	setup(&a, &p);
	setup(&b, &p);
	for (int k = 0; k < 2000; k++) {
		i.pitch = 0.05f * sinf((float)k * 0.01f);
		i.pitch_rate = 0.3f * cosf((float)k * 0.01f);
		i.v_meas = 0.1f * sinf((float)k * 0.003f);
		bb_ctrl_step(&a, &i, DT, &oa);
		bb_ctrl_step(&b, &i, DT, &ob);
		CHECK(oa.u_l == ob.u_l && oa.u_r == ob.u_r);
	}
}

int main(void)
{
	RUN(sign_conventions);
	RUN(speed_request_gives_forward_lean_reference_and_initial_reverse_push);
	RUN(yaw_command_turns_left_with_right_wheel_faster);
	RUN(voltage_normalisation);
	RUN(bad_battery_reading_is_ignored);
	RUN(dead_zone_compensation_is_continuous_and_signed);
	RUN(limiters_hold_for_random_commands);
	RUN(turn_is_reduced_before_balance_when_voltage_is_short);
	RUN(headroom_limiter_cuts_speed_request);
	RUN(reset_clears_state);
	RUN(deterministic);
	TEST_MAIN_END();
}
