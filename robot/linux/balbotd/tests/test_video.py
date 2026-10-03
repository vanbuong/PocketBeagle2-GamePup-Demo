# SPDX-License-Identifier: GPL-2.0-only
import asyncio
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib

import pytest

from balbotd.video import (CommandSource, PatternSource, VideoHub, camera_command, split_jpeg_frames)

HAVE_FFMPEG = shutil.which("ffmpeg") is not None
needs_ffmpeg = pytest.mark.skipif(not HAVE_FFMPEG, reason="ffmpeg not installed")


def ffmpeg_jpegs(n=6, size="160x120", rate=30):
    out = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                          f"testsrc=size={size}:rate={rate}", "-frames:v", str(n), "-c:v", "mjpeg", "-q:v", "8",
                          "-f", "image2pipe", "-"], capture_output=True, check=True).stdout
    return out


@pytest.fixture(scope="module")
def stream():
    if not HAVE_FFMPEG:
        pytest.skip("ffmpeg not installed")
    return ffmpeg_jpegs()


def is_jpeg(b):
    return b[:2] == b"\xff\xd8" and b[-2:] == b"\xff\xd9"


# ---- splitting ---------------------------------------------------------------------------------------------
def test_split_finds_every_frame_whatever_the_chunking(stream):
    whole = bytearray(stream)
    ref = split_jpeg_frames(whole)
    assert len(ref) == 6 and all(is_jpeg(f) for f in ref) and not whole and b"".join(ref) == stream
    for chunk in (1, 7, 100, 4096):
        buf = bytearray()
        got = []
        for i in range(0, len(stream), chunk):
            buf += stream[i:i + chunk]
            got += split_jpeg_frames(buf)
        assert got == ref, chunk


def test_split_skips_garbage_keeps_the_partial_tail_and_handles_a_split_marker(stream):
    one = split_jpeg_frames(bytearray(stream))[0]
    buf = bytearray(b"junk\x00\xff\x01" + one + one[:50])
    assert split_jpeg_frames(buf) == [one]
    assert bytes(buf) == one[:50]
    buf = bytearray(b"\xff")  # first half of an SOI arrives alone
    assert split_jpeg_frames(buf) == [] and bytes(buf) == b"\xff"
    buf += b"\xd8" + one[2:]
    assert split_jpeg_frames(buf) == [one]


def test_split_drops_a_runaway_buffer_and_never_returns_incomplete_frames():
    buf = bytearray(b"\xff\xd8" + b"\x00" * (5 * 1024 * 1024))
    assert split_jpeg_frames(buf) == [] and len(buf) == 0
    buf = bytearray(b"\xff\xd8abc")
    assert split_jpeg_frames(buf) == [] and bytes(buf) == b"\xff\xd8abc"
    assert split_jpeg_frames(bytearray()) == []


def test_camera_command_copies_mjpeg_without_reencoding():
    c = camera_command("/dev/video2", "640x480", 15)
    assert c[0] == "ffmpeg" and "-c:v" in c and c[c.index("-c:v") + 1] == "copy"
    assert c[c.index("-input_format") + 1] == "mjpeg" and c[c.index("-video_size") + 1] == "640x480"
    assert c[c.index("-i") + 1] == "/dev/video2" and c[-1] == "-"


# ---- sources -----------------------------------------------------------------------------------------------
async def take(source, n, timeout=10.0):
    out = []
    agen = source.frames()
    try:
        async def run():
            async for f in agen:
                out.append(f)
                if len(out) >= n:
                    return
        await asyncio.wait_for(run(), timeout)
    finally:
        await agen.aclose()
    return out


@needs_ffmpeg
async def test_command_source_yields_real_jpeg_frames_from_ffmpeg():
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-re", "-f", "lavfi", "-i", "testsrc=size=160x120:rate=25",
           "-c:v", "mjpeg", "-q:v", "8", "-f", "image2pipe", "-"]
    frames = await take(CommandSource(cmd), 8)
    assert len(frames) == 8 and all(is_jpeg(d) and m == "image/jpeg" for d, m in frames)
    assert len({d for d, _ in frames}) > 1, "testsrc animates, frames must differ"


async def test_command_source_restarts_a_command_that_exits_and_reports_errors():
    code = "import sys;sys.stdout.buffer.write(b'\\xff\\xd8AB\\xff\\xd9');sys.stderr.write('boom')"
    src = CommandSource([sys.executable, "-c", code], restart_delay_s=0.05)
    frames = await take(src, 3)
    assert [d for d, _ in frames] == [b"\xff\xd8AB\xff\xd9"] * 3
    assert src.restarts >= 2 and "boom" in src.last_error


async def test_command_source_survives_a_missing_binary():
    src = CommandSource(["/nonexistent/ffmpeg"], restart_delay_s=0.05)
    with pytest.raises(asyncio.TimeoutError):
        await take(src, 1, timeout=0.3)
    assert "cannot start" in src.last_error and src.restarts >= 2


def decode_png(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        tag, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + ln]
        (crc,) = struct.unpack(">I", data[pos + 8 + ln:pos + 12 + ln])
        assert crc == zlib.crc32(tag + body) & 0xFFFFFFFF, tag
        if tag == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat += body
        pos += 12 + ln
    w, h = ihdr[:2]
    raw = zlib.decompress(idat)
    assert len(raw) == h * (1 + w * 3)
    return w, h, raw


def test_pattern_frames_are_valid_png_and_carry_the_frame_number():
    src = PatternSource(320, 180)
    for n in (1, 2, 255, 40000):
        w, h, raw = decode_png(src.render(n))
        assert (w, h) == (320, 180)
        stride = 1 + w * 3
        bits = 0
        for bit in range(16):  # first row, pixel 10*bit+2: white = 1
            px = raw[1 + (10 * bit + 2) * 3]
            bits |= (1 if px == 255 else 0) << bit
        assert bits == n


async def test_pattern_source_rate():
    t0 = time.monotonic()
    frames = await take(PatternSource(64, 36, fps=50), 11)
    assert 0.15 < time.monotonic() - t0 < 0.5 and all(m == "image/png" for _, m in frames)


# ---- hub ---------------------------------------------------------------------------------------------------
class CountingSource:
    def __init__(self, period=0.01, fail_after=None):
        self.period, self.fail_after, self.running, self.starts, self.n = period, fail_after, False, 0, 0

    async def frames(self):
        self.running = True
        self.starts += 1
        try:
            while True:
                self.n += 1
                if self.fail_after and self.n > self.fail_after:
                    raise RuntimeError("camera unplugged")
                yield self.n.to_bytes(4, "big"), "image/jpeg"
                await asyncio.sleep(self.period)
        finally:
            self.running = False


async def test_one_source_serves_many_viewers_and_stops_when_the_last_leaves():
    src = CountingSource()
    hub = VideoHub(src, idle_stop_s=0.1)
    a, b = hub.subscribe(), hub.subscribe()
    fa, fb = await a.next(1), await b.next(1)
    assert fa is not None and fb is not None and src.starts == 1 and hub.stats.subscribers == 2
    a.close()
    await asyncio.sleep(0.2)
    assert src.running, "one viewer is still watching"
    b.close()
    await asyncio.sleep(0.3)
    assert not src.running and hub.latest is None, "source must stop when nobody watches (saves CPU and USB bandwidth)"
    c = hub.subscribe()
    assert await c.next(1) is not None and src.starts == 2
    c.close()
    await hub.stop()


async def test_a_returning_viewer_cancels_the_pending_stop():
    src = CountingSource()
    hub = VideoHub(src, idle_stop_s=0.2)
    a = hub.subscribe()
    await a.next(1)
    a.close()
    await asyncio.sleep(0.1)
    b = hub.subscribe()
    await asyncio.sleep(0.3)
    assert src.running and src.starts == 1
    b.close()
    await hub.stop()


async def test_slow_viewer_only_sees_the_newest_frame():
    hub = VideoHub(CountingSource(period=0.005), idle_stop_s=0.1)
    slow = hub.subscribe()
    await slow.next(1)
    await asyncio.sleep(0.2)  # ~40 frames produced while we were busy
    f = await slow.next(1)
    assert slow.dropped >= 10 and int.from_bytes(f.data, "big") > 20
    slow.close()
    await hub.stop()


async def test_failing_source_is_contained_and_new_viewers_restart_it():
    src = CountingSource(fail_after=3)
    hub = VideoHub(src, idle_stop_s=0.1)
    a = hub.subscribe()
    await asyncio.sleep(0.2)
    assert "unplugged" in hub.stats.last_error and hub.stats.fps == 0.0
    a.close()
    b = hub.subscribe()
    assert hub.stats.starts == 2
    b.close()
    await hub.stop()


async def test_next_times_out_without_frames():
    class Silent:
        async def frames(self):
            await asyncio.sleep(10)
            yield b"", ""
    hub = VideoHub(Silent())
    s = hub.subscribe()
    assert await s.next(0.05) is None
    s.close()
    await hub.stop()


async def test_snapshot_without_viewers_starts_and_then_leaves_the_source_to_idle_out():
    src = CountingSource()
    hub = VideoHub(src, idle_stop_s=0.1)
    f = await hub.snapshot()
    assert f is not None and f.mime == "image/jpeg"
    await asyncio.sleep(0.3)
    assert not src.running
    await hub.stop()
