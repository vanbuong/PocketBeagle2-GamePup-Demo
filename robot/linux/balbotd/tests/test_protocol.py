# SPDX-License-Identifier: GPL-2.0-only
import ctypes
import os
import random
import struct

import pytest

from balbotd import protocol as P

LIB = os.environ.get("BALBOT_LIB") or os.path.join(os.path.dirname(__file__), "..", "..", "..", "core", "build-shared",
                                                  "libbalbot.so")


def test_crc_check_vectors():
    assert P.crc16(b"123456789") == 0x29B1
    assert P.crc8(b"123456789") == 0xF4
    assert P.crc16(b"") == 0xFFFF and P.crc8(b"") == 0


def test_golden_frame_bytes():
    f = P.encode(P.CMD_DRIVE, 0x0102, b"\xaa\x55")
    head = bytes([0xB5, 1, 1, 0, 0x02, 0x01, 0x02, 0x00, 0xAA, 0x55])
    assert f == head + struct.pack("<H", P.crc16(head))
    assert P.decode(f) == P.Frame(P.CMD_DRIVE, 0x0102, b"\xaa\x55", 0)


@pytest.mark.parametrize("n", [0, 1, 6, 20, 479, 480])
def test_round_trip_lengths(n):
    payload = bytes(range(256)) * 2
    f = P.encode(P.TLM_FAST, 0xFFFF, payload[:n], flags=1)
    fr = P.decode(f)
    assert (fr.type, fr.seq, fr.flags, fr.payload) == (P.TLM_FAST, 0xFFFF, 1, payload[:n])


def test_decode_rejects_bad_frames():
    good = P.encode(P.CMD_DRIVE, 7, b"1234")
    for bad in (good[:5], good[:-1], good + b"\0"):
        with pytest.raises(P.ProtocolError):
            P.decode(bad)
    for idx, val in ((0, 0xB4), (1, 9)):
        b = bytearray(good)
        b[idx] = val
        with pytest.raises(P.ProtocolError):
            P.decode(bytes(b))
    b = bytearray(good)
    b[8] ^= 0x10
    with pytest.raises(P.ProtocolError, match="crc"):
        P.decode(bytes(b))
    with pytest.raises(P.ProtocolError):
        P.encode(1, 1, bytes(481))
    huge = bytearray(good)
    huge[6:8] = b"\xff\xff"
    with pytest.raises(P.ProtocolError):
        P.decode(bytes(huge))


def test_single_bit_flips_are_always_detected():
    good = P.encode(P.TLM_FAST, 5, bytes(range(20)))
    for i in range(len(good)):
        for bit in range(8):
            b = bytearray(good)
            b[i] ^= 1 << bit
            with pytest.raises(P.ProtocolError):
                P.decode(bytes(b))


def test_payload_round_trips():
    assert P.unpack_cmd_drive(P.pack_cmd_drive(-1234, 32767, 3, 9)) == (-1234, 32767, 3, 9)
    t = P.TlmFast(0xDEADBEEF, -3500, 1234, -4321, -600, 9000, -9000, 11100, 3, 0x41)
    assert P.TlmFast.unpack(t.pack()) == t
    assert P.unpack_cfg_set(P.pack_cfg_set(0x1234, -12.5)) == (0x1234, -12.5)
    assert P.unpack_cfg_val(P.pack_cfg_val(7, 3.25, P.CFG_OUT_OF_RANGE)) == (7, 3.25, P.CFG_OUT_OF_RANGE)
    assert P.unpack_cfg_get(P.pack_cfg_get(33)) == 33
    assert P.unpack_evt_state(P.pack_evt_state(3, 0x14)) == (3, 0x14)
    assert P.unpack_heartbeat(P.pack_heartbeat(0xA1B2C3D4)) == 0xA1B2C3D4
    for fn, bad in ((P.unpack_cmd_drive, b"12345"), (P.TlmFast.unpack, b"1" * 19), (P.unpack_cfg_set, b"1" * 5),
                    (P.unpack_cfg_val, b"1" * 6), (P.unpack_evt_state, b"1"), (P.unpack_heartbeat, b"123"),
                    (P.unpack_cfg_get, b"1")):
        with pytest.raises(P.ProtocolError):
            fn(bad)


def test_ble_control_frame():
    b = P.pack_ble_ctrl(65535, -1000, 1000, 0x81)
    assert len(b) == 8 and P.unpack_ble_ctrl(b) == (65535, -1000, 1000, 0x81)
    for i in range(8):
        for bit in range(8):
            x = bytearray(b)
            x[i] ^= 1 << bit
            with pytest.raises(P.ProtocolError):
                P.unpack_ble_ctrl(bytes(x))


def test_sequence_tracker_matches_c_rules():
    s = P.SeqTracker()
    assert [s.check(x) for x in (65534, 65535, 0, 0, 65535, 5, 6)] == ["ok", "ok", "ok", "dup", "dup", "gap", "ok"]
    assert (s.gaps, s.dups) == (1, 2)


# ---- cross-language: the C library is the reference ---------------------------------------------------

class CFrame(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint8), ("flags", ctypes.c_uint8), ("seq", ctypes.c_uint16),
                ("len", ctypes.c_uint16), ("payload", ctypes.POINTER(ctypes.c_uint8))]


@pytest.fixture(scope="module")
def clib():
    if not os.path.exists(LIB):
        pytest.skip("libbalbot.so not built")
    lib = ctypes.CDLL(LIB)
    u8p = ctypes.POINTER(ctypes.c_uint8)
    lib.bb_frame_encode.argtypes = [u8p, ctypes.c_size_t, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint16, u8p,
                                    ctypes.c_uint16]
    lib.bb_frame_decode.argtypes = [u8p, ctypes.c_size_t, ctypes.POINTER(CFrame)]
    lib.bb_pack_cfg_val.argtypes = [u8p, ctypes.c_size_t, ctypes.c_void_p]
    lib.bb_pack_cfg_set.argtypes = [u8p, ctypes.c_size_t, ctypes.c_void_p]
    lib.bb_pack_tlm_fast.argtypes = [u8p, ctypes.c_size_t, ctypes.c_void_p]
    lib.bb_pack_cmd_drive.argtypes = [u8p, ctypes.c_size_t, ctypes.c_void_p]
    return lib


def _buf(b: bytes):
    return (ctypes.c_uint8 * max(1, len(b))).from_buffer_copy(b or b"\0")


def test_python_frames_decode_in_c_and_vice_versa(clib):
    rng = random.Random(1)
    for _ in range(300):
        n = rng.randrange(0, 481)
        payload = bytes(rng.randrange(256) for _ in range(n))
        typ, flags, seq = rng.randrange(256), rng.randrange(256), rng.randrange(65536)
        py = P.encode(typ, seq, payload, flags)
        out = (ctypes.c_uint8 * 600)()
        k = clib.bb_frame_encode(out, 600, typ, flags, seq, _buf(payload), n)
        assert bytes(out[:k]) == py
        cf = CFrame()
        pybuf = _buf(py)  # cf.payload points into this buffer: keep it alive while reading
        assert clib.bb_frame_decode(pybuf, len(py), ctypes.byref(cf)) == 0
        assert (cf.type, cf.flags, cf.seq, cf.len) == (typ, flags, seq, n)
        assert bytes(cf.payload[:n]) == payload


def test_c_and_python_agree_on_which_corrupt_frames_are_rejected(clib):
    rng = random.Random(2)
    good = P.encode(P.TLM_FAST, 9, bytes(range(20)))
    cf = CFrame()
    for _ in range(500):
        b = bytearray(good)
        for _ in range(rng.randrange(1, 4)):
            b[rng.randrange(len(b))] ^= 1 << rng.randrange(8)
        if rng.random() < 0.3:
            b = b[: rng.randrange(len(b))]
        c_ok = clib.bb_frame_decode(_buf(bytes(b)), len(b), ctypes.byref(cf)) == 0
        try:
            P.decode(bytes(b))
            py_ok = True
        except P.ProtocolError:
            py_ok = False
        assert c_ok == py_ok


def test_payload_packers_match_c(clib):
    class CfgVal(ctypes.Structure):
        _fields_ = [("key", ctypes.c_uint16), ("value", ctypes.c_float), ("status", ctypes.c_uint8)]

    class CfgSet(ctypes.Structure):
        _fields_ = [("key", ctypes.c_uint16), ("value", ctypes.c_float)]

    class Tlm(ctypes.Structure):
        _fields_ = [("t_us", ctypes.c_uint32), ("pitch", ctypes.c_int16), ("pr", ctypes.c_int16),
                    ("yr", ctypes.c_int16), ("v", ctypes.c_int16), ("ul", ctypes.c_int16), ("ur", ctypes.c_int16),
                    ("vbat", ctypes.c_uint16), ("state", ctypes.c_uint8), ("faults", ctypes.c_uint8)]

    class Drive(ctypes.Structure):
        _fields_ = [("v", ctypes.c_int16), ("w", ctypes.c_int16), ("mode", ctypes.c_uint8), ("epoch", ctypes.c_uint8)]

    out = (ctypes.c_uint8 * 32)()
    assert clib.bb_pack_cfg_val(out, 32, ctypes.byref(CfgVal(7, 3.25, 3))) == 7
    assert bytes(out[:7]) == P.pack_cfg_val(7, 3.25, 3)
    assert clib.bb_pack_cfg_set(out, 32, ctypes.byref(CfgSet(0x1234, -12.5))) == 6
    assert bytes(out[:6]) == P.pack_cfg_set(0x1234, -12.5)
    assert clib.bb_pack_tlm_fast(out, 32, ctypes.byref(Tlm(0xDEADBEEF, -3500, 1234, -4321, -600, 9000, -9000, 11100,
                                                           3, 0x41))) == 20
    assert bytes(out[:20]) == P.TlmFast(0xDEADBEEF, -3500, 1234, -4321, -600, 9000, -9000, 11100, 3, 0x41).pack()
    assert clib.bb_pack_cmd_drive(out, 32, ctypes.byref(Drive(-1234, 32767, 3, 9))) == 6
    assert bytes(out[:6]) == P.pack_cmd_drive(-1234, 32767, 3, 9)
