/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "balbot/crc.h"
#include "balbot/proto.h"
#include "balbot/state.h"
#include "tst.h"

#define BB_ST_BALANCING_FOR_TEST BB_ST_BALANCING

static void round_trip_frame(void)
{
	uint8_t buf[600], pl[BB_FRAME_MAX_PAYLOAD];
	struct bb_frame f;

	for (int i = 0; i < BB_FRAME_MAX_PAYLOAD; i++)
		pl[i] = (uint8_t)i;
	int lens[] = {0, 1, 6, 20, 479, 480};

	for (unsigned k = 0; k < sizeof lens / sizeof lens[0]; k++) {
		int n = bb_frame_encode(buf, sizeof buf, BB_TLM_FAST, 1, 0xFFFF, pl, (uint16_t)lens[k]);

		CHECK_EQ(n, BB_FRAME_HDR + lens[k] + BB_FRAME_CRC);
		CHECK_EQ(bb_frame_decode(buf, (size_t)n, &f), BB_OK);
		CHECK_EQ(f.type, BB_TLM_FAST);
		CHECK_EQ(f.flags, 1);
		CHECK_EQ(f.seq, 0xFFFF);
		CHECK_EQ(f.len, lens[k]);
		CHECK(lens[k] == 0 || memcmp(f.payload, pl, (size_t)lens[k]) == 0);
	}
}

static void golden_frame_bytes(void)
{
	/* header: B5 01 01 00 | seq 0x0102 -> 02 01 | len 2 -> 02 00 | payload AA 55 | crc16 */
	uint8_t buf[16];
	const uint8_t pl[2] = {0xAA, 0x55};
	int n = bb_frame_encode(buf, sizeof buf, BB_CMD_DRIVE, 0, 0x0102, pl, 2);
	const uint8_t head[10] = {0xB5, 0x01, 0x01, 0x00, 0x02, 0x01, 0x02, 0x00, 0xAA, 0x55};
	uint16_t crc = bb_crc16(head, 10);

	CHECK_EQ(n, 12);
	CHECK(memcmp(buf, head, 10) == 0);
	CHECK_EQ(buf[10], crc & 0xFF);
	CHECK_EQ(buf[11], crc >> 8);
}

static void rejects_bad_frames(void)
{
	uint8_t buf[64];
	struct bb_frame f;
	const uint8_t pl[4] = {1, 2, 3, 4};
	int n = bb_frame_encode(buf, sizeof buf, BB_CMD_DRIVE, 0, 7, pl, 4);

	CHECK_EQ(bb_frame_decode(buf, 5, &f), BB_ERR_SHORT);
	CHECK_EQ(bb_frame_decode(buf, (size_t)n - 1, &f), BB_ERR_SHORT);
	CHECK_EQ(bb_frame_decode(buf, (size_t)n + 1, &f), BB_ERR_LEN);
	buf[0] ^= 1;
	CHECK_EQ(bb_frame_decode(buf, (size_t)n, &f), BB_ERR_MAGIC);
	buf[0] ^= 1;
	buf[1] = 9;
	CHECK_EQ(bb_frame_decode(buf, (size_t)n, &f), BB_ERR_VERSION);
	buf[1] = BB_PROTO_VER;
	buf[8] ^= 0x10;
	CHECK_EQ(bb_frame_decode(buf, (size_t)n, &f), BB_ERR_CRC);
	buf[8] ^= 0x10;
	CHECK_EQ(bb_frame_decode(buf, (size_t)n, &f), BB_OK);
	CHECK_EQ(bb_frame_decode(NULL, 10, &f), BB_ERR_ARG);
	CHECK_EQ(bb_frame_encode(buf, 4, BB_CMD_DRIVE, 0, 1, pl, 4), BB_ERR_CAP);
	CHECK_EQ(bb_frame_encode(buf, sizeof buf, 1, 0, 1, pl, BB_FRAME_MAX_PAYLOAD + 1), BB_ERR_LEN);
}

static void oversize_length_field_is_safe(void)
{
	uint8_t buf[16] = {BB_FRAME_MAGIC, BB_PROTO_VER, 1, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0};
	struct bb_frame f;

	CHECK_EQ(bb_frame_decode(buf, sizeof buf, &f), BB_ERR_LEN);
}

static void fuzz_never_crashes_or_accepts_garbage(void)
{
	uint8_t buf[600];
	struct bb_frame f;
	int accepted = 0;

	for (int i = 0; i < 200000; i++) {
		size_t n = tst_rand() % 40;

		for (size_t j = 0; j < n; j++)
			buf[j] = (uint8_t)tst_rand();
		if (n > 2 && (tst_rand() & 1)) {
			buf[0] = BB_FRAME_MAGIC;
			buf[1] = BB_PROTO_VER;
		}
		if (bb_frame_decode(buf, n, &f) == BB_OK)
			accepted++;
	}
	CHECK(accepted < 20); /* only random CRC collisions with a valid header may pass */
}

static void cmd_drive_pack(void)
{
	uint8_t b[BB_CMD_DRIVE_LEN];
	struct bb_cmd_drive in = {-1234, 32767, 3, 9}, out;

	CHECK_EQ(bb_pack_cmd_drive(b, sizeof b, &in), BB_CMD_DRIVE_LEN);
	CHECK_EQ(bb_unpack_cmd_drive(b, sizeof b, &out), BB_OK);
	CHECK_EQ(out.v_mm_s, -1234);
	CHECK_EQ(out.w_mrad_s, 32767);
	CHECK_EQ(out.mode, 3);
	CHECK_EQ(out.lease_epoch, 9);
	CHECK_EQ(bb_unpack_cmd_drive(b, 5, &out), BB_ERR_LEN);
	CHECK_EQ(bb_pack_cmd_drive(b, 5, &in), BB_ERR_CAP);
	in.v_mm_s = -32768;
	bb_pack_cmd_drive(b, sizeof b, &in);
	bb_unpack_cmd_drive(b, sizeof b, &out);
	CHECK_EQ(out.v_mm_s, -32768);
}

static void tlm_fast_pack(void)
{
	uint8_t b[BB_TLM_FAST_LEN];
	struct bb_tlm_fast in = {0xDEADBEEFu, -3500, 1234, -4321, -600, 9000, -9000, 11100, 3, 0x41}, out;

	CHECK_EQ(bb_pack_tlm_fast(b, sizeof b, &in), BB_TLM_FAST_LEN);
	CHECK_EQ(bb_unpack_tlm_fast(b, sizeof b, &out), BB_OK);
	CHECK(memcmp(&in, &out, sizeof in) == 0);
	CHECK_EQ(b[0], 0xEF); /* little endian */
	CHECK_EQ(b[3], 0xDE);
}

static void cfg_messages(void)
{
	uint8_t b[16];
	struct bb_cfg_set s = {0x1234, -12.5f}, so;
	struct bb_cfg_val v = {7, 3.25f, BB_CFG_OUT_OF_RANGE}, vo;
	struct bb_evt_state e = {BB_ST_BALANCING_FOR_TEST, 0x14}, eo;
	uint32_t up = 0;

	CHECK_EQ(bb_pack_cfg_set(b, sizeof b, &s), BB_CFG_SET_LEN);
	CHECK_EQ(b[0], 0x34);
	CHECK_EQ(b[1], 0x12);
	CHECK_EQ(bb_unpack_cfg_set(b, BB_CFG_SET_LEN, &so), BB_OK);
	CHECK_EQ(so.key, 0x1234);
	CHECK(so.value == -12.5f);
	CHECK_EQ(bb_unpack_cfg_set(b, 5, &so), BB_ERR_LEN);
	CHECK_EQ(bb_pack_cfg_set(b, 5, &s), BB_ERR_CAP);
	/* -12.5f is 0xC1480000 little endian after the key */
	CHECK_EQ(b[2], 0x00);
	CHECK_EQ(b[3], 0x00);
	CHECK_EQ(b[4], 0x48);
	CHECK_EQ(b[5], 0xC1);
	CHECK_EQ(bb_pack_cfg_val(b, sizeof b, &v), BB_CFG_VAL_LEN);
	CHECK_EQ(bb_unpack_cfg_val(b, BB_CFG_VAL_LEN, &vo), BB_OK);
	CHECK_EQ(vo.key, 7);
	CHECK(vo.value == 3.25f);
	CHECK_EQ(vo.status, BB_CFG_OUT_OF_RANGE);
	CHECK_EQ(bb_unpack_cfg_val(b, 6, &vo), BB_ERR_LEN);
	CHECK_EQ(bb_pack_evt_state(b, sizeof b, &e), BB_EVT_STATE_LEN);
	CHECK_EQ(bb_unpack_evt_state(b, BB_EVT_STATE_LEN, &eo), BB_OK);
	CHECK_EQ(eo.state, BB_ST_BALANCING_FOR_TEST);
	CHECK_EQ(eo.faults, 0x14);
	CHECK_EQ(bb_pack_heartbeat(b, sizeof b, 0xA1B2C3D4u), BB_HEARTBEAT_LEN);
	CHECK_EQ(b[0], 0xD4);
	CHECK_EQ(bb_unpack_heartbeat(b, 4, &up), BB_OK);
	CHECK_EQ(up, 0xA1B2C3D4u);
	CHECK_EQ(bb_unpack_heartbeat(b, 3, &up), BB_ERR_LEN);
}

static void ble_frame(void)
{
	uint8_t b[BB_BLE_CTRL_LEN];
	struct bb_ble_ctrl in = {65535, -1000, 1000, 0x81}, out;

	CHECK_EQ(bb_ble_ctrl_pack(b, &in), BB_OK);
	CHECK_EQ(bb_ble_ctrl_unpack(b, &out), BB_OK);
	CHECK_EQ(out.seq, 65535);
	CHECK_EQ(out.vx_milli, -1000);
	CHECK_EQ(out.wz_milli, 1000);
	CHECK_EQ(out.flags, 0x81);
	for (int i = 0; i < 8; i++)
		for (int bit = 0; bit < 8; bit++) {
			b[i] ^= (uint8_t)(1u << bit);
			CHECK_EQ(bb_ble_ctrl_unpack(b, &out), BB_ERR_CRC);
			b[i] ^= (uint8_t)(1u << bit);
		}
}

static void sequence_tracking(void)
{
	struct bb_seq s;

	bb_seq_init(&s);
	CHECK_EQ(bb_seq_check(&s, 65534), BB_SEQ_OK);
	CHECK_EQ(bb_seq_check(&s, 65535), BB_SEQ_OK);
	CHECK_EQ(bb_seq_check(&s, 0), BB_SEQ_OK);      /* wrap */
	CHECK_EQ(bb_seq_check(&s, 0), BB_SEQ_DUP);
	CHECK_EQ(bb_seq_check(&s, 65535), BB_SEQ_DUP); /* old frame after wrap */
	CHECK_EQ(bb_seq_check(&s, 5), BB_SEQ_GAP);
	CHECK_EQ(s.gaps, 1);
	CHECK_EQ(s.dups, 2);
	CHECK_EQ(bb_seq_check(&s, 6), BB_SEQ_OK);
}

int main(void)
{
	RUN(round_trip_frame);
	RUN(golden_frame_bytes);
	RUN(rejects_bad_frames);
	RUN(oversize_length_field_is_safe);
	RUN(fuzz_never_crashes_or_accepts_garbage);
	RUN(cmd_drive_pack);
	RUN(tlm_fast_pack);
	RUN(cfg_messages);
	RUN(ble_frame);
	RUN(sequence_tracking);
	TEST_MAIN_END();
}
