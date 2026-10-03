/* SPDX-License-Identifier: GPL-2.0-only */
/* A53 <-> M4F rpmsg frames (docs/balance-robot/01-system-architecture.md, 1.5) and the
 * 8-byte BLE control frame (03-remote-control.md, 3.4). All integers little endian. */
#ifndef BALBOT_PROTO_H
#define BALBOT_PROTO_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BB_FRAME_MAGIC 0xB5
#define BB_PROTO_VER 1
#define BB_FRAME_HDR 8
#define BB_FRAME_CRC 2
#define BB_FRAME_MAX_PAYLOAD 480

enum bb_msg_type {
	BB_CMD_DRIVE = 0x01,
	BB_CMD_ARM = 0x02,
	BB_CMD_DISARM = 0x03,
	BB_CMD_ESTOP = 0x04,
	BB_CMD_HEARTBEAT = 0x05,
	BB_CFG_SET = 0x10,
	BB_CFG_GET = 0x11,
	BB_CFG_SAVE = 0x12,
	BB_CAL_START = 0x20,
	BB_EVT_STATE = 0x80,
	BB_TLM_FAST = 0x81,
	BB_TLM_SLOW = 0x82,
	BB_CFG_VAL = 0x90,
	BB_CAL_RESULT = 0xA0,
};

enum bb_err {
	BB_OK = 0,
	BB_ERR_SHORT = -1,
	BB_ERR_MAGIC = -2,
	BB_ERR_VERSION = -3,
	BB_ERR_LEN = -4,
	BB_ERR_CRC = -5,
	BB_ERR_CAP = -6,
	BB_ERR_ARG = -7,
};

struct bb_frame {
	uint8_t type;
	uint8_t flags;
	uint16_t seq;
	uint16_t len;
	const uint8_t *payload; /* points into the decoded buffer */
};

/* Returns total frame bytes (>0) or a negative bb_err. */
int bb_frame_encode(uint8_t *buf, size_t cap, uint8_t type, uint8_t flags, uint16_t seq,
		    const uint8_t *payload, uint16_t len);
/* Returns BB_OK or a negative bb_err. Never reads past n bytes. */
int bb_frame_decode(const uint8_t *buf, size_t n, struct bb_frame *out);

/* ---- payloads ---- */
struct bb_cmd_drive {
	int16_t v_mm_s;
	int16_t w_mrad_s;
	uint8_t mode;
	uint8_t lease_epoch;
};
#define BB_CMD_DRIVE_LEN 6
int bb_pack_cmd_drive(uint8_t *buf, size_t cap, const struct bb_cmd_drive *c);
int bb_unpack_cmd_drive(const uint8_t *buf, size_t n, struct bb_cmd_drive *c);

struct bb_tlm_fast {
	uint32_t t_us;
	int16_t pitch_cdeg;     /* 0.01 deg */
	int16_t pitch_rate_dps10; /* 0.1 deg/s */
	int16_t yaw_rate_dps10;
	int16_t v_mm_s;
	int16_t u_l_mv;
	int16_t u_r_mv;
	uint16_t vbat_mv;
	uint8_t state;
	uint8_t faults;
};
#define BB_TLM_FAST_LEN 20
int bb_pack_tlm_fast(uint8_t *buf, size_t cap, const struct bb_tlm_fast *t);
int bb_unpack_tlm_fast(const uint8_t *buf, size_t n, struct bb_tlm_fast *t);

/* CFG_SET (A->M): u16 key, f32 value. CFG_VAL (M->A, reply to CFG_GET and CFG_SET): adds a status byte. */
struct bb_cfg_set {
	uint16_t key;
	float value;
};
#define BB_CFG_SET_LEN 6
int bb_pack_cfg_set(uint8_t *buf, size_t cap, const struct bb_cfg_set *c);
int bb_unpack_cfg_set(const uint8_t *buf, size_t n, struct bb_cfg_set *c);

enum bb_cfg_status { BB_CFG_OK = 0, BB_CFG_REJECTED = 1, BB_CFG_UNKNOWN_KEY = 2, BB_CFG_OUT_OF_RANGE = 3 };
struct bb_cfg_val {
	uint16_t key;
	float value;
	uint8_t status;
};
#define BB_CFG_VAL_LEN 7
int bb_pack_cfg_val(uint8_t *buf, size_t cap, const struct bb_cfg_val *c);
int bb_unpack_cfg_val(const uint8_t *buf, size_t n, struct bb_cfg_val *c);

/* EVT_STATE (M->A): u8 state (enum bb_state), u8 fault bits (enum bb_fault, low 8 bits) */
struct bb_evt_state {
	uint8_t state;
	uint8_t faults;
};
#define BB_EVT_STATE_LEN 2
int bb_pack_evt_state(uint8_t *buf, size_t cap, const struct bb_evt_state *e);
int bb_unpack_evt_state(const uint8_t *buf, size_t n, struct bb_evt_state *e);

/* CMD_HEARTBEAT (A->M): u32 uptime_ms */
#define BB_HEARTBEAT_LEN 4
int bb_pack_heartbeat(uint8_t *buf, size_t cap, uint32_t uptime_ms);
int bb_unpack_heartbeat(const uint8_t *buf, size_t n, uint32_t *uptime_ms);

/* ---- BLE control frame: u16 seq, i16 vx, i16 wz (x1/1000), u8 flags, u8 crc8 ---- */
struct bb_ble_ctrl {
	uint16_t seq;
	int16_t vx_milli;
	int16_t wz_milli;
	uint8_t flags;
};
#define BB_BLE_CTRL_LEN 8
int bb_ble_ctrl_pack(uint8_t buf[BB_BLE_CTRL_LEN], const struct bb_ble_ctrl *c);
int bb_ble_ctrl_unpack(const uint8_t buf[BB_BLE_CTRL_LEN], struct bb_ble_ctrl *c);

/* ---- sequence tracking ---- */
enum bb_seq_result { BB_SEQ_OK = 0, BB_SEQ_DUP = 1, BB_SEQ_GAP = 2 };
struct bb_seq {
	uint16_t last;
	int started;
	uint32_t gaps;
	uint32_t dups;
};
void bb_seq_init(struct bb_seq *s);
/* Wrap-safe. Frames "behind" the last one (within half the range) are duplicates/old. */
enum bb_seq_result bb_seq_check(struct bb_seq *s, uint16_t seq);

#ifdef __cplusplus
}
#endif
#endif
