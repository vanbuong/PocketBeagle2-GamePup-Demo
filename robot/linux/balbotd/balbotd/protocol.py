# SPDX-License-Identifier: GPL-2.0-only
"""Python mirror of robot/core/include/balbot/proto.h (A53 <-> M4F rpmsg frames).

Layout, little endian: magic 0xB5, ver, type, flags, seq u16, len u16, payload, crc16/CCITT-FALSE.
tests/test_protocol.py checks this module against the C implementation (libbalbot.so) when it is built.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = 0xB5
VERSION = 1
HDR = 8
CRC_LEN = 2
MAX_PAYLOAD = 480

# message types
CMD_DRIVE = 0x01
CMD_ARM = 0x02
CMD_DISARM = 0x03
CMD_ESTOP = 0x04
CMD_HEARTBEAT = 0x05
CFG_SET = 0x10
CFG_GET = 0x11
CFG_SAVE = 0x12
CAL_START = 0x20
EVT_STATE = 0x80
TLM_FAST = 0x81
TLM_SLOW = 0x82
CFG_VAL = 0x90
CAL_RESULT = 0xA0

# robot states (enum bb_state)
STATES = ["BOOT", "CALIBRATING", "STANDBY", "BALANCING", "LAYING_DOWN", "FALLEN", "FAULT"]
ST_BOOT, ST_CALIBRATING, ST_STANDBY, ST_BALANCING, ST_LAYING_DOWN, ST_FALLEN, ST_FAULT = range(7)

# fault bits (enum bb_fault)
FAULT_IMU, FAULT_OVERRUN, FAULT_ESTOP, FAULT_ENCODER, FAULT_STALL, FAULT_PRU, FAULT_CAL = (1 << i for i in range(7))

# CFG status
CFG_OK, CFG_REJECTED, CFG_UNKNOWN_KEY, CFG_OUT_OF_RANGE = range(4)


class ProtocolError(ValueError):
    """Frame or payload rejected (bad magic/version/length/CRC)."""


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


@dataclass(frozen=True)
class Frame:
    type: int
    seq: int
    payload: bytes = b""
    flags: int = 0


def encode(type_: int, seq: int, payload: bytes = b"", flags: int = 0) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ProtocolError("payload too long")
    head = struct.pack("<BBBBHH", MAGIC, VERSION, type_, flags, seq & 0xFFFF, len(payload))
    body = head + payload
    return body + struct.pack("<H", crc16(body))


def decode(data: bytes) -> Frame:
    if len(data) < HDR + CRC_LEN:
        raise ProtocolError("short frame")
    magic, ver, type_, flags, seq, length = struct.unpack_from("<BBBBHH", data)
    if magic != MAGIC:
        raise ProtocolError("bad magic")
    if ver != VERSION:
        raise ProtocolError("bad version")
    if length > MAX_PAYLOAD:
        raise ProtocolError("bad length")
    if len(data) != HDR + length + CRC_LEN:
        raise ProtocolError("length mismatch")
    (crc,) = struct.unpack_from("<H", data, HDR + length)
    if crc != crc16(data[: HDR + length]):
        raise ProtocolError("bad crc")
    return Frame(type_, seq, bytes(data[HDR : HDR + length]), flags)


# ---- payloads ------------------------------------------------------------------

def pack_cmd_drive(v_mm_s: int, w_mrad_s: int, mode: int = 0, lease_epoch: int = 0) -> bytes:
    return struct.pack("<hhBB", v_mm_s, w_mrad_s, mode, lease_epoch)


def unpack_cmd_drive(p: bytes) -> tuple[int, int, int, int]:
    if len(p) != 6:
        raise ProtocolError("CMD_DRIVE length")
    return struct.unpack("<hhBB", p)


@dataclass(frozen=True)
class TlmFast:
    t_us: int
    pitch_cdeg: int
    pitch_rate_dps10: int
    yaw_rate_dps10: int
    v_mm_s: int
    u_l_mv: int
    u_r_mv: int
    vbat_mv: int
    state: int
    faults: int

    _FMT = "<IhhhhhhHBB"

    def pack(self) -> bytes:
        return struct.pack(self._FMT, self.t_us & 0xFFFFFFFF, self.pitch_cdeg, self.pitch_rate_dps10,
                           self.yaw_rate_dps10, self.v_mm_s, self.u_l_mv, self.u_r_mv, self.vbat_mv,
                           self.state, self.faults)

    @classmethod
    def unpack(cls, p: bytes) -> "TlmFast":
        if len(p) != 20:
            raise ProtocolError("TLM_FAST length")
        return cls(*struct.unpack(cls._FMT, p))


def pack_cfg_set(key: int, value: float) -> bytes:
    return struct.pack("<Hf", key, value)


def unpack_cfg_set(p: bytes) -> tuple[int, float]:
    if len(p) != 6:
        raise ProtocolError("CFG_SET length")
    return struct.unpack("<Hf", p)


def pack_cfg_get(key: int) -> bytes:
    return struct.pack("<H", key)


def unpack_cfg_get(p: bytes) -> int:
    if len(p) != 2:
        raise ProtocolError("CFG_GET length")
    return struct.unpack("<H", p)[0]


def pack_cfg_val(key: int, value: float, status: int = CFG_OK) -> bytes:
    return struct.pack("<HfB", key, value, status)


def unpack_cfg_val(p: bytes) -> tuple[int, float, int]:
    if len(p) != 7:
        raise ProtocolError("CFG_VAL length")
    return struct.unpack("<HfB", p)


def pack_evt_state(state: int, faults: int) -> bytes:
    return struct.pack("<BB", state, faults & 0xFF)


def unpack_evt_state(p: bytes) -> tuple[int, int]:
    if len(p) != 2:
        raise ProtocolError("EVT_STATE length")
    return struct.unpack("<BB", p)


def pack_heartbeat(uptime_ms: int) -> bytes:
    return struct.pack("<I", uptime_ms & 0xFFFFFFFF)


def unpack_heartbeat(p: bytes) -> int:
    if len(p) != 4:
        raise ProtocolError("HEARTBEAT length")
    return struct.unpack("<I", p)[0]


def pack_ble_ctrl(seq: int, vx_milli: int, wz_milli: int, flags: int) -> bytes:
    body = struct.pack("<HhhB", seq & 0xFFFF, vx_milli, wz_milli, flags)
    return body + bytes([crc8(body)])


def unpack_ble_ctrl(b: bytes) -> tuple[int, int, int, int]:
    if len(b) != 8:
        raise ProtocolError("BLE frame length")
    if b[7] != crc8(b[:7]):
        raise ProtocolError("BLE crc")
    return struct.unpack("<HhhB", b[:7])


class SeqTracker:
    """Wrap-safe sequence tracking, same rules as bb_seq_check()."""

    def __init__(self) -> None:
        self.last = 0
        self.started = False
        self.gaps = 0
        self.dups = 0

    def check(self, seq: int) -> str:
        if not self.started:
            self.started = True
            self.last = seq
            return "ok"
        diff = (seq - self.last) & 0xFFFF
        if diff == 0 or diff >= 0x8000:
            self.dups += 1
            return "dup"
        self.last = seq
        if diff > 1:
            self.gaps += 1
            return "gap"
        return "ok"
