/* SPDX-License-Identifier: GPL-2.0-only */
/* MPU-6500 helpers: init sequence, burst parsing, unit conversion, health, dt, gyro bias
 * calibration. Register values follow docs/balance-robot/04-imu-mpu6500.md and must be
 * re-verified against the InvenSense datasheet before first use on hardware. */
#ifndef BALBOT_IMU_H
#define BALBOT_IMU_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BB_PI 3.14159265358979323846f
#define BB_G0 9.80665f

/* register map (subset) */
#define MPU_REG_SMPLRT_DIV 0x19
#define MPU_REG_CONFIG 0x1A
#define MPU_REG_GYRO_CONFIG 0x1B
#define MPU_REG_ACCEL_CONFIG 0x1C
#define MPU_REG_ACCEL_CONFIG2 0x1D
#define MPU_REG_INT_PIN_CFG 0x37
#define MPU_REG_INT_ENABLE 0x38
#define MPU_REG_ACCEL_XOUT_H 0x3B
#define MPU_REG_SIGNAL_PATH_RESET 0x68
#define MPU_REG_USER_CTRL 0x6A
#define MPU_REG_PWR_MGMT_1 0x6B
#define MPU_REG_PWR_MGMT_2 0x6C
#define MPU_REG_WHO_AM_I 0x75
#define MPU6500_WHO_AM_I_VALUE 0x70

#define MPU_ACCEL_LSB_PER_G 8192.0f   /* +-4 g  */
#define MPU_GYRO_LSB_PER_DPS 32.8f    /* +-1000 dps */
#define MPU_TEMP_LSB_PER_C 333.87f

enum bb_imu_op_kind {
	BB_IMU_OP_WRITE,        /* write val to reg, then wait delay_ms */
	BB_IMU_OP_EXPECT,       /* read reg, must equal val (else fault) */
	BB_IMU_OP_SPI_KHZ,      /* switch SPI clock to val_khz */
	BB_IMU_OP_DELAY,        /* wait delay_ms */
	BB_IMU_OP_DISCARD,      /* discard `val_khz` samples (settling) */
};

struct bb_imu_op {
	uint8_t kind;
	uint8_t reg;
	uint8_t val;
	uint16_t delay_ms;
	uint16_t arg; /* kHz or sample count */
};

/* Returns the constant init sequence and its length. */
const struct bb_imu_op *bb_mpu6500_init_seq(size_t *n);

struct bb_imu_raw {
	int16_t ax, ay, az, temp, gx, gy, gz;
};

/* rx[0] is the byte clocked while the address was sent; rx[1..14] are ax..gz big endian. */
int bb_mpu6500_parse(const uint8_t rx[15], struct bb_imu_raw *o);

/* ---- orientation: 24 proper rotations of the axes ---- */
#define BB_AXIS_ORIENTATIONS 24
/* R maps sensor axes to body axes (x forward, y left, z up): body = R * sensor. Entries are 0, +-1. */
int bb_axis_rotation(int idx, int8_t R[3][3]);

struct bb_imu_cal {
	int8_t R[3][3];
	float accel_offset_g[3];
	float accel_scale[3];
	float gyro_bias_rads[3];
};

struct bb_imu_si {
	float a_g[3];      /* body frame, g */
	float w_rads[3];   /* body frame, rad/s, bias removed */
	float temp_c;
};

void bb_imu_cal_default(struct bb_imu_cal *c, int orientation_idx);
void bb_imu_convert(const struct bb_imu_raw *raw, const struct bb_imu_cal *cal, struct bb_imu_si *out);

/* ---- health ---- */
struct bb_imu_health {
	uint8_t last[14];
	int have_last;
	uint32_t same_count;
	uint32_t bad_run;
	int fault;
	uint32_t bad_total;
};
void bb_imu_health_init(struct bb_imu_health *h);
/* Feed the 14 data bytes (rx+1). Fault latches after 3 consecutive bad frames
 * (all 0x00, all 0xFF, or 50 identical frames in a row). Returns 1 if this frame was bad. */
int bb_imu_health_update(struct bb_imu_health *h, const uint8_t data14[14]);

/* ---- sample interval filter (DRDY timestamps in microseconds) ---- */
struct bb_dt_filter {
	float dt;        /* filtered interval, seconds */
	uint32_t last_us;
	int started;
	uint32_t missed; /* intervals > 1.5x nominal */
	float nominal;
};
void bb_dt_init(struct bb_dt_filter *f, float nominal_s);
float bb_dt_update(struct bb_dt_filter *f, uint32_t now_us);

/* ---- gyro bias calibration at rest ---- */
enum bb_cal_state { BB_CAL_RUNNING = 0, BB_CAL_DONE = 1, BB_CAL_FAILED = 2 };
struct bb_gyro_cal {
	uint32_t window;      /* samples to average (1024) */
	uint32_t timeout;     /* total samples before FAILED */
	float max_std_rads;   /* motion limit per axis */
	float a_min_g, a_max_g;
	uint32_t n, total, restarts;
	double sum[3], sumsq[3];
	float bias[3];
	int state;
};
void bb_gyro_cal_init(struct bb_gyro_cal *c, uint32_t window, uint32_t timeout);
int bb_gyro_cal_feed(struct bb_gyro_cal *c, const float w_rads[3], float accel_mag_g);

#ifdef __cplusplus
}
#endif
#endif
