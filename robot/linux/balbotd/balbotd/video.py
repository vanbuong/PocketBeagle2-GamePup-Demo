# SPDX-License-Identifier: GPL-2.0-only
"""MJPEG video (doc 02, V1): camera JPEG frames are forwarded, never decoded or re-encoded.

A source yields complete frames. The hub runs one source for any number of viewers, gives every viewer only the
newest frame (a slow viewer drops frames instead of adding latency) and stops the source when nobody watches.
"""
from __future__ import annotations

import asyncio
import contextlib
import struct
import time
import zlib
from dataclasses import dataclass, field
from typing import AsyncIterator, Callable, Optional, Protocol

SOI = b"\xff\xd8"
EOI = b"\xff\xd9"
MAX_FRAME = 4 * 1024 * 1024


@dataclass(frozen=True)
class Frame:
    data: bytes
    mime: str  # image/jpeg from cameras, image/png from the built-in test pattern
    n: int     # running frame number of the producer


class Source(Protocol):
    def frames(self) -> AsyncIterator[tuple[bytes, str]]: ...


# ---- JPEG splitting ---------------------------------------------------------------------------------------
def split_jpeg_frames(buf: bytearray) -> list[bytes]:
    """Remove and return all complete JPEG frames (SOI..EOI) at the start of buf; keep the partial tail.

    Bytes before the first SOI are discarded. Entropy-coded JPEG data never contains FF D9 (0xFF is stuffed as
    FF 00), so the next EOI ends the frame; UVC MJPEG frames carry no embedded thumbnails. A buffer that grows
    past MAX_FRAME without completing a frame is dropped.
    """
    out = []
    while True:
        s = buf.find(SOI)
        if s < 0:
            # keep a trailing 0xFF: it may be the first half of an SOI split across reads
            del buf[: len(buf) - 1 if buf.endswith(b"\xff") else len(buf)]
            break
        if s:
            del buf[:s]
        e = buf.find(EOI, 2)
        if e < 0:
            if len(buf) > MAX_FRAME:
                buf.clear()
            break
        out.append(bytes(buf[: e + 2]))
        del buf[: e + 2]
    return out


# ---- sources ------------------------------------------------------------------------------------------------
def camera_command(device: str = "/dev/video0", size: str = "1280x720", fps: int = 30) -> list[str]:
    """ffmpeg copying the camera's MJPG stream (no decode, no encode) to stdout as concatenated JPEGs."""
    return ["ffmpeg", "-hide_banner", "-loglevel", "error", "-fflags", "nobuffer", "-flags", "low_delay",
            "-f", "v4l2", "-input_format", "mjpeg", "-video_size", size, "-framerate", str(fps), "-i", device,
            "-c:v", "copy", "-f", "image2pipe", "-"]


class CommandSource:
    """Runs a command that writes concatenated JPEG frames to stdout; restarts it if it exits."""

    def __init__(self, argv: list[str], restart_delay_s: float = 1.0):
        self.argv = argv
        self.restart_delay_s = restart_delay_s
        self.restarts = 0
        self.last_error = ""

    async def frames(self):
        while True:
            try:
                proc = await asyncio.create_subprocess_exec(*self.argv, stdout=asyncio.subprocess.PIPE,
                                                            stderr=asyncio.subprocess.PIPE)
            except OSError as e:
                self.last_error = f"cannot start {self.argv[0]}: {e}"
                await asyncio.sleep(self.restart_delay_s)
                self.restarts += 1
                continue
            buf = bytearray()
            try:
                while True:
                    chunk = await proc.stdout.read(65536)
                    if not chunk:
                        break
                    buf += chunk
                    for f in split_jpeg_frames(buf):
                        yield f, "image/jpeg"
            finally:
                if proc.returncode is None:
                    with contextlib.suppress(ProcessLookupError):
                        proc.kill()
                err = b""
                with contextlib.suppress(Exception):
                    err = await asyncio.wait_for(proc.stderr.read(2000), 0.5)
                await proc.wait()
                if err:
                    self.last_error = err.decode("utf-8", "replace").strip()[-300:]
            self.restarts += 1
            await asyncio.sleep(self.restart_delay_s)


def _png(width: int, height: int, rows: list[bytes]) -> bytes:
    def chunk(tag: bytes, body: bytes) -> bytes:
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + r for r in rows)  # filter type 0 on every row
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b""))


class PatternSource:
    """A moving test pattern (no camera, no ffmpeg needed): grid, a sweeping bar and a frame-number strip."""

    def __init__(self, width: int = 320, height: int = 180, fps: float = 15.0):
        self.w, self.h, self.fps = width, height, fps

    def render(self, n: int) -> bytes:
        w, h = self.w, self.h
        bar_x = (n * 6) % w
        rows = []
        for y in range(h):
            row = bytearray()
            for x in range(w):
                if abs(x - bar_x) < 3:
                    px = (255, 190, 0)
                elif x % 40 == 0 or y % 40 == 0:
                    px = (70, 80, 95)
                else:
                    px = (22 + x * 40 // w, 28 + y * 40 // h, 46)
                row += bytes(px)
            rows.append(bytes(row))
        # frame number as 16 bits along the top edge, 10 px per bit: readable by a test
        for bit in range(16):
            on = (n >> bit) & 1
            for y in range(4):
                r = bytearray(rows[y])
                for x in range(bit * 10, bit * 10 + 8):
                    if x < w:
                        r[x * 3: x * 3 + 3] = bytes((255, 255, 255) if on else (0, 0, 0))
                rows[y] = bytes(r)
        return _png(w, h, rows)

    async def frames(self):
        n = 0
        t_next = time.monotonic()
        while True:
            n += 1
            yield self.render(n), "image/png"
            t_next += 1.0 / self.fps
            await asyncio.sleep(max(0.0, t_next - time.monotonic()))


# ---- hub ----------------------------------------------------------------------------------------------------
class Subscription:
    def __init__(self, hub: "VideoHub"):
        self._hub = hub
        self._frame: Optional[Frame] = None
        self._event = asyncio.Event()
        self.closed = False
        self.dropped = 0

    def _put(self, f: Frame) -> None:
        if self._frame is not None:
            self.dropped += 1  # the viewer did not keep up: it only ever sees the newest frame
        self._frame = f
        self._event.set()

    async def next(self, timeout: float) -> Optional[Frame]:
        """Newest frame not yet delivered, or None after `timeout` seconds without one."""
        try:
            await asyncio.wait_for(self._event.wait(), timeout)
        except asyncio.TimeoutError:
            return None
        f, self._frame = self._frame, None
        self._event.clear()
        return f

    def close(self) -> None:
        if not self.closed:
            self.closed = True
            self._hub._unsubscribe(self)


@dataclass
class VideoStats:
    frames: int = 0
    bytes: int = 0
    subscribers: int = 0
    starts: int = 0
    fps: float = 0.0
    last_error: str = ""


class VideoHub:
    def __init__(self, source: Source, idle_stop_s: float = 3.0, clock: Callable[[], float] = time.monotonic):
        self.source = source
        self.idle_stop_s = idle_stop_s
        self.clock = clock
        self.stats = VideoStats()
        self._subs: set[Subscription] = set()
        self._latest: Optional[Frame] = None
        self._task: Optional[asyncio.Task] = None
        self._idle_task: Optional[asyncio.Task] = None

    @property
    def latest(self) -> Optional[Frame]:
        return self._latest

    def _ensure_running(self) -> None:
        if self._idle_task:
            self._idle_task.cancel()
            self._idle_task = None
        if self._task is None or self._task.done():
            self.stats.starts += 1
            self._task = asyncio.get_running_loop().create_task(self._run())

    async def _run(self) -> None:
        n = 0
        t_last = self.clock()
        try:
            async for data, mime in self.source.frames():
                n += 1
                f = Frame(data, mime, n)
                self._latest = f
                self.stats.frames += 1
                self.stats.bytes += len(data)
                now = self.clock()
                dt = now - t_last
                t_last = now
                if dt > 0:
                    self.stats.fps += 0.1 * (1.0 / dt - self.stats.fps)
                for s in list(self._subs):
                    s._put(f)
        except asyncio.CancelledError:
            raise
        except Exception as e:  # a failing source must not take the server down
            self.stats.last_error = repr(e)
        finally:
            self.stats.fps = 0.0
            err = getattr(self.source, "last_error", "")
            if err:
                self.stats.last_error = err

    def subscribe(self) -> Subscription:
        s = Subscription(self)
        self._subs.add(s)
        self.stats.subscribers = len(self._subs)
        self._ensure_running()
        return s

    def _unsubscribe(self, s: Subscription) -> None:
        self._subs.discard(s)
        self.stats.subscribers = len(self._subs)
        if not self._subs and self._task and not self._task.done() and self._idle_task is None:
            self._idle_task = asyncio.get_running_loop().create_task(self._stop_when_idle())

    async def _stop_when_idle(self) -> None:
        try:
            await asyncio.sleep(self.idle_stop_s)
        except asyncio.CancelledError:
            return
        if not self._subs and self._task:
            self._task.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await self._task
            self._task = None
            self._latest = None
        self._idle_task = None

    async def snapshot(self, timeout: float = 3.0) -> Optional[Frame]:
        """One frame: the cached newest one, or briefly start the source and wait for the first."""
        if self._latest is not None and self._task and not self._task.done():
            return self._latest
        sub = self.subscribe()
        try:
            return await sub.next(timeout)
        finally:
            sub.close()

    async def stop(self) -> None:
        for t in (self._idle_task, self._task):
            if t:
                t.cancel()
        for t in (self._idle_task, self._task):
            if t:
                with contextlib.suppress(asyncio.CancelledError, Exception):
                    await t
        self._task = self._idle_task = None
