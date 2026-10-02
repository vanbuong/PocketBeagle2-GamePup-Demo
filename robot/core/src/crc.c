/* SPDX-License-Identifier: GPL-2.0-only */
#include "balbot/crc.h"

uint16_t bb_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFF;

	for (size_t i = 0; i < len; i++) {
		crc = (uint16_t)(crc ^ (uint16_t)((uint16_t)data[i] << 8));
		for (int b = 0; b < 8; b++) {
			uint16_t sh = (uint16_t)(crc << 1);

			crc = (crc & 0x8000) ? (uint16_t)(sh ^ 0x1021) : sh;
		}
	}
	return crc;
}

uint8_t bb_crc8(const uint8_t *data, size_t len)
{
	uint8_t crc = 0;

	for (size_t i = 0; i < len; i++) {
		crc = (uint8_t)(crc ^ data[i]);
		for (int b = 0; b < 8; b++) {
			uint8_t sh = (uint8_t)(crc << 1);

			crc = (crc & 0x80) ? (uint8_t)(sh ^ 0x07) : sh;
		}
	}
	return crc;
}
