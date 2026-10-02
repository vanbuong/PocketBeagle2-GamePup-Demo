/* SPDX-License-Identifier: GPL-2.0-only */
/* Safety/operating state machine (doc 05, 5.7). Evaluated every control tick BEFORE the
 * control calculation; the motors may only be driven when bb_sm_motors_active() is true. */
#ifndef BALBOT_STATE_H
#define BALBOT_STATE_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum bb_state {
	BB_ST_BOOT = 0,
	BB_ST_CALIBRATING,
	BB_ST_STANDBY,
	BB_ST_BALANCING,
	BB_ST_LAYING_DOWN,
	BB_ST_FALLEN,
	BB_ST_FAULT,
	BB_ST_COUNT
};

enum bb_fault {
	BB_FAULT_IMU = 1 << 0,
	BB_FAULT_OVERRUN = 1 << 1,
	BB_FAULT_ESTOP = 1 << 2,
	BB_FAULT_ENCODER = 1 << 3,
	BB_FAULT_STALL = 1 << 4,
	BB_FAULT_PRU = 1 << 5,
	BB_FAULT_CAL = 1 << 6,
};

enum bb_cmd {
	BB_SMCMD_ARM,
	BB_SMCMD_DISARM,
	BB_SMCMD_ESTOP,
	BB_SMCMD_RESET,
};

struct bb_sm_cfg {
	float tip_rad;          /* 35 deg: balancing -> FALLEN */
	float arm_rad;          /* 5 deg: pitch window to arm / to leave FALLEN */
	float arm_hold_s;       /* 1 s inside the window */
	float arm_timeout_s;    /* 10 s pending ARM request */
	float fallen_hold_s;    /* 2 s upright before FALLEN -> STANDBY */
	float link_lost_s;      /* heartbeat missing this long while balancing -> LAYING_DOWN (10 s) */
	float laydown_rad;      /* 70 deg: LAYING_DOWN -> STANDBY */
	float laydown_timeout_s;
	float vbat_low;         /* volts (filtered): refuse ARM, lay down below this */
	float vbat_cut;         /* volts: disarm */
};

struct bb_sm_in {
	float pitch;            /* rad */
	float vbat;             /* volts, filtered */
	bool imu_ok;            /* data fresh and healthy */
	bool link_ok;           /* heartbeat from Linux fresh (< 500 ms) */
	bool estop_active;      /* hardware E-stop pressed / loop open */
	bool cal_done;
	bool cal_failed;
	uint32_t fault_bits;    /* new fault conditions raised this tick (IMU, overrun, ...) */
};

struct bb_sm {
	struct bb_sm_cfg cfg;
	enum bb_state state;
	uint32_t faults;        /* latched */
	bool arm_pending;
	float arm_pending_t;
	float upright_t;
	float link_lost_t;
	float state_t;
	bool disarm_seen;       /* in FALLEN: a DISARM must be seen before leaving */
	uint32_t refused;       /* counter of refused commands */
};

void bb_sm_cfg_default(struct bb_sm_cfg *c);
void bb_sm_init(struct bb_sm *s, const struct bb_sm_cfg *cfg);
/* Returns true if accepted. DISARM and ESTOP are accepted in every state. */
bool bb_sm_command(struct bb_sm *s, enum bb_cmd cmd, const struct bb_sm_in *in);
void bb_sm_step(struct bb_sm *s, const struct bb_sm_in *in, float dt);
bool bb_sm_motors_active(const struct bb_sm *s);
const char *bb_state_name(enum bb_state st);

#ifdef __cplusplus
}
#endif
#endif
