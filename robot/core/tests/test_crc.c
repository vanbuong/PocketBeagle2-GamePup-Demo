/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "balbot/crc.h"
#include "tst.h"

static void check_vectors(void)
{
	const char *s = "123456789";

	CHECK_EQ(bb_crc16((const uint8_t *)s, 9), 0x29B1);
	CHECK_EQ(bb_crc8((const uint8_t *)s, 9), 0xF4);
	CHECK_EQ(bb_crc16((const uint8_t *)"", 0), 0xFFFF);
	CHECK_EQ(bb_crc8((const uint8_t *)"", 0), 0x00);
}

static void detects_single_bit_flips(void)
{
	uint8_t d[32];

	for (int i = 0; i < 32; i++)
		d[i] = (uint8_t)(i * 7 + 3);
	uint16_t c16 = bb_crc16(d, sizeof d);
	uint8_t c8 = bb_crc8(d, sizeof d);

	for (int byte = 0; byte < 32; byte++)
		for (int bit = 0; bit < 8; bit++) {
			d[byte] ^= (uint8_t)(1u << bit);
			CHECK(bb_crc16(d, sizeof d) != c16);
			CHECK(bb_crc8(d, sizeof d) != c8);
			d[byte] ^= (uint8_t)(1u << bit);
		}
}

int main(void)
{
	RUN(check_vectors);
	RUN(detects_single_bit_flips);
	TEST_MAIN_END();
}
