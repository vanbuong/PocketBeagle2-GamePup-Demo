/* SPDX-License-Identifier: GPL-2.0-only */
/* M4F application skeleton (doc 01, 1.4). UNBUILT and UNTESTED on hardware: the hardware access
 * functions below are stubs marked TODO(M1/M2). The control path itself is the host-tested core.
 *
 * imu_ctrl runs on every MPU-6500 data-ready interrupt (1 kHz): read, convert, estimate, and every
 * second tick run the balance controller and write PRU1. Nothing in it blocks or allocates. */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "balbot/balance.h"
#include "balbot/estimator.h"
#include "balbot/imu.h"
#include "balbot/mailbox.h"
#include "balbot/proto.h"
#include "balbot/speed.h"
#include "balbot/state.h"

LOG_MODULE_REGISTER(balbot, LOG_LEVEL_INF);

#define CONTROL_DIV 2 /* 1 kHz IMU, 500 Hz control */

static K_SEM_DEFINE(drdy_sem, 0, 1);
static volatile uint32_t drdy_cycles;

static struct bb_imu_cal cal;
static struct bb_cf att;
static struct bb_ctrl ctrl;
static struct bb_sm sm;
static struct bb_speed_win spd_l, spd_r;
static struct bb_dt_filter dtf;

/* TODO(M1): map these to the PRU data RAM addresses from the remoteproc/devicetree resource table */
static struct bb_pru1_cmd *const pru1 = NULL;
static const struct bb_pru0_out *const pru0 = NULL;

static int imu_read_burst(uint8_t rx[15]) { (void)rx; return -1; /* TODO(M2): SPI burst read, 4 MHz */ }
static uint32_t now_us(void) { return k_cyc_to_us_floor32(k_cycle_get_32()); }

/* TODO(M2): GPIO callback on IMU_INT rising edge */
static void imu_drdy_isr(void)
{
	drdy_cycles = k_cycle_get_32();
	k_sem_give(&drdy_sem);
}

static void imu_ctrl_thread(void *a, void *b, void *c)
{
	uint32_t tick = 0;

	(void)a; (void)b; (void)c;
	for (;;) {
		uint8_t rx[15];
		struct bb_imu_raw raw;
		struct bb_imu_si si;
		float dt;

		k_sem_take(&drdy_sem, K_FOREVER);
		if (imu_read_burst(rx) != 0)
			continue; /* TODO: count the failure, feed bb_imu_health_update */
		bb_mpu6500_parse(rx, &raw);
		bb_imu_convert(&raw, &cal, &si);
		dt = bb_dt_update(&dtf, now_us());
		bb_cf_update(&att, si.w_rads[1], si.a_g[0], si.a_g[1], si.a_g[2], dt);

		if (++tick % CONTROL_DIV == 0 && pru1 && pru0) {
			struct bb_pru0_out snap;
			struct bb_ctrl_in in = {.pitch = att.pitch, .pitch_rate = att.rate, .yaw_rate = si.w_rads[2],
						.v_meas = 0.0f /* TODO: 100 Hz speed estimate from snap */, .vbat = 11.1f};
			struct bb_ctrl_out out;

			(void)bb_pru0_snapshot(pru0, &snap, 3);
			bb_ctrl_step(&ctrl, &in, dt * CONTROL_DIV, &out);
			pru1->duty_l = (int16_t)(bb_sm_motors_active(&sm) ? out.duty_l * 1000.0f : 0.0f);
			pru1->duty_r = (int16_t)(bb_sm_motors_active(&sm) ? out.duty_r * 1000.0f : 0.0f);
			pru1->mode = bb_sm_motors_active(&sm) ? BB_PRU1_DRIVE : BB_PRU1_BRAKE;
			pru1->heartbeat++;
		}
	}
}

K_THREAD_DEFINE(imu_ctrl, 2048, imu_ctrl_thread, NULL, NULL, NULL, K_PRIO_COOP(2), 0, 0);

int main(void)
{
	struct bb_params p;
	struct bb_sm_cfg smc;

	bb_imu_cal_default(&cal, 0);
	bb_cf_init(&att, 4.0f, 0.03f); /* tau and gate chosen in simulation, doc 05 5.12 */
	bb_params_default(&p);
	bb_ctrl_init(&ctrl, &p);
	bb_sm_cfg_default(&smc);
	bb_sm_init(&sm, &smc);
	bb_speed_win_init(&spd_l, 0.065f * BB_PI / 1320.0f, 0.5f);
	bb_speed_win_init(&spd_r, 0.065f * BB_PI / 1320.0f, 0.5f);
	bb_dt_init(&dtf, 0.001f);
	LOG_INF("balbot M4F skeleton up (protocol v%d)", BB_PROTO_VER);
	(void)imu_drdy_isr;
	return 0; /* TODO(M1/M2): init SPI, GPIO IRQ, IMU init sequence, rpmsg endpoint, safety thread */
}
