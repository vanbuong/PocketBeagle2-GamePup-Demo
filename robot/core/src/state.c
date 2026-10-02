/* SPDX-License-Identifier: GPL-2.0-only */
#include "balbot/state.h"

#include <math.h>
#include <string.h>

#include "balbot/imu.h"

void bb_sm_cfg_default(struct bb_sm_cfg *c)
{
	c->tip_rad = 35.0f * BB_PI / 180.0f;
	c->arm_rad = 5.0f * BB_PI / 180.0f;
	c->arm_hold_s = 1.0f;
	c->arm_timeout_s = 10.0f;
	c->fallen_hold_s = 2.0f;
	c->link_lost_s = 10.0f;
	c->laydown_rad = 70.0f * BB_PI / 180.0f;
	c->laydown_timeout_s = 5.0f;
	c->vbat_low = 9.9f;  /* 3.3 V per cell, 3S */
	c->vbat_cut = 9.6f;
}

void bb_sm_init(struct bb_sm *s, const struct bb_sm_cfg *cfg)
{
	memset(s, 0, sizeof(*s));
	s->cfg = *cfg;
	s->state = BB_ST_BOOT;
}

static void enter(struct bb_sm *s, enum bb_state st)
{
	s->state = st;
	s->state_t = 0.0f;
	s->arm_pending = false;
	s->arm_pending_t = 0.0f;
	s->upright_t = 0.0f;
	s->link_lost_t = 0.0f;
	s->disarm_seen = false;
}

static void fault(struct bb_sm *s, uint32_t bits)
{
	s->faults |= bits;
	if (s->state != BB_ST_FAULT)
		enter(s, BB_ST_FAULT);
}

bool bb_sm_command(struct bb_sm *s, enum bb_cmd cmd, const struct bb_sm_in *in)
{
	switch (cmd) {
	case BB_SMCMD_ESTOP:
		fault(s, BB_FAULT_ESTOP);
		return true;
	case BB_SMCMD_DISARM:
		s->arm_pending = false;
		if (s->state == BB_ST_BALANCING || s->state == BB_ST_LAYING_DOWN)
			enter(s, BB_ST_STANDBY);
		if (s->state == BB_ST_FALLEN)
			s->disarm_seen = true;
		return true;
	case BB_SMCMD_ARM:
		if (s->state == BB_ST_STANDBY && s->faults == 0 && in->imu_ok && !in->estop_active &&
		    in->vbat >= s->cfg.vbat_low) {
			s->arm_pending = true;
			s->arm_pending_t = 0.0f;
			s->upright_t = 0.0f;
			return true;
		}
		s->refused++;
		return false;
	case BB_SMCMD_RESET:
		if (s->state == BB_ST_FAULT && !in->estop_active && in->fault_bits == 0) {
			s->faults = 0;
			enter(s, BB_ST_BOOT);
			return true;
		}
		s->refused++;
		return false;
	}
	return false;
}

void bb_sm_step(struct bb_sm *s, const struct bb_sm_in *in, float dt)
{
	float ap = fabsf(in->pitch);

	s->state_t += dt;

	/* safety conditions first, in every state */
	if (in->estop_active) {
		fault(s, BB_FAULT_ESTOP);
		return;
	}
	if (in->fault_bits) {
		fault(s, in->fault_bits);
		return;
	}
	if (s->state != BB_ST_BOOT && s->state != BB_ST_FAULT && !in->imu_ok) {
		fault(s, BB_FAULT_IMU);
		return;
	}

	switch (s->state) {
	case BB_ST_BOOT:
		if (in->imu_ok)
			enter(s, BB_ST_CALIBRATING);
		break;
	case BB_ST_CALIBRATING:
		if (in->cal_failed)
			fault(s, BB_FAULT_CAL);
		else if (in->cal_done)
			enter(s, BB_ST_STANDBY);
		break;
	case BB_ST_STANDBY:
		if (s->arm_pending) {
			s->arm_pending_t += dt;
			if (ap < s->cfg.arm_rad && in->vbat >= s->cfg.vbat_low)
				s->upright_t += dt;
			else
				s->upright_t = 0.0f;
			if (s->upright_t >= s->cfg.arm_hold_s)
				enter(s, BB_ST_BALANCING);
			else if (s->arm_pending_t >= s->cfg.arm_timeout_s)
				s->arm_pending = false;
		}
		break;
	case BB_ST_BALANCING:
		if (ap > s->cfg.tip_rad) {
			enter(s, BB_ST_FALLEN);
			break;
		}
		if (in->vbat < s->cfg.vbat_cut) {
			enter(s, BB_ST_STANDBY);
			break;
		}
		s->link_lost_t = in->link_ok ? 0.0f : s->link_lost_t + dt;
		if (s->link_lost_t >= s->cfg.link_lost_s || in->vbat < s->cfg.vbat_low)
			enter(s, BB_ST_LAYING_DOWN);
		break;
	case BB_ST_LAYING_DOWN:
		if (ap > s->cfg.laydown_rad || s->state_t >= s->cfg.laydown_timeout_s)
			enter(s, BB_ST_STANDBY);
		break;
	case BB_ST_FALLEN:
		if (ap < s->cfg.arm_rad)
			s->upright_t += dt;
		else
			s->upright_t = 0.0f;
		if (s->upright_t >= s->cfg.fallen_hold_s && s->disarm_seen)
			enter(s, BB_ST_STANDBY);
		break;
	case BB_ST_FAULT:
	default:
		break;
	}
}

bool bb_sm_motors_active(const struct bb_sm *s)
{
	return s->state == BB_ST_BALANCING || s->state == BB_ST_LAYING_DOWN;
}

const char *bb_state_name(enum bb_state st)
{
	static const char *const names[BB_ST_COUNT] = {"BOOT", "CALIBRATING", "STANDBY", "BALANCING",
							"LAYING_DOWN", "FALLEN", "FAULT"};

	return (unsigned)st < BB_ST_COUNT ? names[st] : "?";
}
