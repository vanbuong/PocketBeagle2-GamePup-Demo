/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BALBOT_CRC_H
#define BALBOT_CRC_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection. check("123456789") = 0x29B1 */
uint16_t bb_crc16(const uint8_t *data, size_t len);
/* CRC-8 (poly 0x07, init 0x00). check("123456789") = 0xF4 */
uint8_t bb_crc8(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
#endif
