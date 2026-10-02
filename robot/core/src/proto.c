/* SPDX-License-Identifier: GPL-2.0-only */
#include "balbot/proto.h"

#include <string.h>

#include "balbot/crc.h"

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)(v & 0xFFFF));
	put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

int bb_frame_encode(uint8_t *buf, size_t cap, uint8_t type, uint8_t flags, uint16_t seq,
		    const uint8_t *payload, uint16_t len)
{
	size_t total = (size_t)BB_FRAME_HDR + len + BB_FRAME_CRC;

	if (!buf || (len && !payload))
		return BB_ERR_ARG;
	if (len > BB_FRAME_MAX_PAYLOAD)
		return BB_ERR_LEN;
	if (cap < total)
		return BB_ERR_CAP;
	buf[0] = BB_FRAME_MAGIC;
	buf[1] = BB_PROTO_VER;
	buf[2] = type;
	buf[3] = flags;
	put16(buf + 4, seq);
	put16(buf + 6, len);
	if (len)
		memcpy(buf + BB_FRAME_HDR, payload, len);
	put16(buf + BB_FRAME_HDR + len, bb_crc16(buf, BB_FRAME_HDR + len));
	return (int)total;
}

int bb_frame_decode(const uint8_t *buf, size_t n, struct bb_frame *out)
{
	uint16_t len;

	if (!buf || !out)
		return BB_ERR_ARG;
	if (n < (size_t)BB_FRAME_HDR + BB_FRAME_CRC)
		return BB_ERR_SHORT;
	if (buf[0] != BB_FRAME_MAGIC)
		return BB_ERR_MAGIC;
	if (buf[1] != BB_PROTO_VER)
		return BB_ERR_VERSION;
	len = get16(buf + 6);
	if (len > BB_FRAME_MAX_PAYLOAD)
		return BB_ERR_LEN;
	if (n != (size_t)BB_FRAME_HDR + len + BB_FRAME_CRC)
		return n < (size_t)BB_FRAME_HDR + len + BB_FRAME_CRC ? BB_ERR_SHORT : BB_ERR_LEN;
	if (get16(buf + BB_FRAME_HDR + len) != bb_crc16(buf, BB_FRAME_HDR + len))
		return BB_ERR_CRC;
	out->type = buf[2];
	out->flags = buf[3];
	out->seq = get16(buf + 4);
	out->len = len;
	out->payload = buf + BB_FRAME_HDR;
	return BB_OK;
}

int bb_pack_cmd_drive(uint8_t *buf, size_t cap, const struct bb_cmd_drive *c)
{
	if (!buf || !c)
		return BB_ERR_ARG;
	if (cap < BB_CMD_DRIVE_LEN)
		return BB_ERR_CAP;
	put16(buf, (uint16_t)c->v_mm_s);
	put16(buf + 2, (uint16_t)c->w_mrad_s);
	buf[4] = c->mode;
	buf[5] = c->lease_epoch;
	return BB_CMD_DRIVE_LEN;
}

int bb_unpack_cmd_drive(const uint8_t *buf, size_t n, struct bb_cmd_drive *c)
{
	if (!buf || !c)
		return BB_ERR_ARG;
	if (n != BB_CMD_DRIVE_LEN)
		return BB_ERR_LEN;
	c->v_mm_s = (int16_t)get16(buf);
	c->w_mrad_s = (int16_t)get16(buf + 2);
	c->mode = buf[4];
	c->lease_epoch = buf[5];
	return BB_OK;
}

int bb_pack_tlm_fast(uint8_t *buf, size_t cap, const struct bb_tlm_fast *t)
{
	if (!buf || !t)
		return BB_ERR_ARG;
	if (cap < BB_TLM_FAST_LEN)
		return BB_ERR_CAP;
	put32(buf, t->t_us);
	put16(buf + 4, (uint16_t)t->pitch_cdeg);
	put16(buf + 6, (uint16_t)t->pitch_rate_dps10);
	put16(buf + 8, (uint16_t)t->yaw_rate_dps10);
	put16(buf + 10, (uint16_t)t->v_mm_s);
	put16(buf + 12, (uint16_t)t->u_l_mv);
	put16(buf + 14, (uint16_t)t->u_r_mv);
	put16(buf + 16, t->vbat_mv);
	buf[18] = t->state;
	buf[19] = t->faults;
	return BB_TLM_FAST_LEN;
}

int bb_unpack_tlm_fast(const uint8_t *buf, size_t n, struct bb_tlm_fast *t)
{
	if (!buf || !t)
		return BB_ERR_ARG;
	if (n != BB_TLM_FAST_LEN)
		return BB_ERR_LEN;
	t->t_us = get32(buf);
	t->pitch_cdeg = (int16_t)get16(buf + 4);
	t->pitch_rate_dps10 = (int16_t)get16(buf + 6);
	t->yaw_rate_dps10 = (int16_t)get16(buf + 8);
	t->v_mm_s = (int16_t)get16(buf + 10);
	t->u_l_mv = (int16_t)get16(buf + 12);
	t->u_r_mv = (int16_t)get16(buf + 14);
	t->vbat_mv = get16(buf + 16);
	t->state = buf[18];
	t->faults = buf[19];
	return BB_OK;
}

int bb_ble_ctrl_pack(uint8_t buf[BB_BLE_CTRL_LEN], const struct bb_ble_ctrl *c)
{
	if (!buf || !c)
		return BB_ERR_ARG;
	put16(buf, c->seq);
	put16(buf + 2, (uint16_t)c->vx_milli);
	put16(buf + 4, (uint16_t)c->wz_milli);
	buf[6] = c->flags;
	buf[7] = bb_crc8(buf, 7);
	return BB_OK;
}

int bb_ble_ctrl_unpack(const uint8_t buf[BB_BLE_CTRL_LEN], struct bb_ble_ctrl *c)
{
	if (!buf || !c)
		return BB_ERR_ARG;
	if (buf[7] != bb_crc8(buf, 7))
		return BB_ERR_CRC;
	c->seq = get16(buf);
	c->vx_milli = (int16_t)get16(buf + 2);
	c->wz_milli = (int16_t)get16(buf + 4);
	c->flags = buf[6];
	return BB_OK;
}

void bb_seq_init(struct bb_seq *s)
{
	memset(s, 0, sizeof(*s));
}

enum bb_seq_result bb_seq_check(struct bb_seq *s, uint16_t seq)
{
	uint16_t diff;

	if (!s->started) {
		s->started = 1;
		s->last = seq;
		return BB_SEQ_OK;
	}
	diff = (uint16_t)(seq - s->last);
	if (diff == 0 || diff >= 0x8000) { /* same or behind */
		s->dups++;
		return BB_SEQ_DUP;
	}
	s->last = seq;
	if (diff > 1) {
		s->gaps++;
		return BB_SEQ_GAP;
	}
	return BB_SEQ_OK;
}
