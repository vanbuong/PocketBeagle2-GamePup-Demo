# SPDX-License-Identifier: GPL-2.0-only
"""Message-oriented links to the M4F.

A Link carries whole rpmsg frames (never partial ones): the rpmsg char device and a SOCK_SEQPACKET unix
socket both preserve message boundaries, and MemoryLink does the same for tests. send() never blocks:
when the transport is full the frame is dropped and counted (doc 01: lossy, non-blocking IPC).
"""
from __future__ import annotations

import asyncio
import errno
import os
import socket
from typing import Callable, Optional


class LinkClosed(Exception):
    pass


class Link:
    sent = 0
    dropped = 0
    received = 0

    async def send(self, data: bytes) -> bool:  # pragma: no cover - interface
        raise NotImplementedError

    async def recv(self) -> bytes:  # pragma: no cover - interface
        raise NotImplementedError

    def close(self) -> None:  # pragma: no cover - interface
        raise NotImplementedError

    @property
    def closed(self) -> bool:  # pragma: no cover - interface
        raise NotImplementedError


_CLOSE = object()


class MemoryLink(Link):
    """In-process link end. `MemoryLink.pair()` gives two connected ends.

    Fault injection on the *sending* end: drop_filter(data)->bool drops a frame, mutate(data)->bytes edits it.
    """

    def __init__(self) -> None:
        self._rx: asyncio.Queue = asyncio.Queue()
        self._peer: Optional["MemoryLink"] = None
        self._closed = False
        self.drop_filter: Optional[Callable[[bytes], bool]] = None
        self.mutate: Optional[Callable[[bytes], bytes]] = None
        self.sent = self.dropped = self.received = 0

    @classmethod
    def pair(cls) -> tuple["MemoryLink", "MemoryLink"]:
        a, b = cls(), cls()
        a._peer, b._peer = b, a
        return a, b

    async def send(self, data: bytes) -> bool:
        if self._closed or self._peer is None or self._peer._closed:
            raise LinkClosed()
        if self.drop_filter and self.drop_filter(data):
            self.dropped += 1
            return False
        if self.mutate:
            data = self.mutate(data)
        self.sent += 1
        self._peer._rx.put_nowait(bytes(data))
        return True

    async def recv(self) -> bytes:
        item = await self._rx.get()
        if item is _CLOSE:
            self._rx.put_nowait(_CLOSE)  # stay closed for every later recv
            raise LinkClosed()
        self.received += 1
        return item

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._rx.put_nowait(_CLOSE)
        if self._peer is not None and not self._peer._closed:
            self._peer._rx.put_nowait(_CLOSE)

    @property
    def closed(self) -> bool:
        return self._closed


class FdLink(Link):
    """A non-blocking file descriptor with message semantics: /dev/rpmsgN or a SOCK_SEQPACKET socket."""

    MAX_MSG = 512

    def __init__(self, fd: int, loop: Optional[asyncio.AbstractEventLoop] = None) -> None:
        self.fd = fd
        self.loop = loop or asyncio.get_running_loop()
        self._rx: asyncio.Queue = asyncio.Queue()
        self._closed = False
        self.sent = self.dropped = self.received = 0
        os.set_blocking(fd, False)
        self.loop.add_reader(fd, self._on_readable)

    @classmethod
    def open_rpmsg(cls, path: str) -> "FdLink":
        return cls(os.open(path, os.O_RDWR | os.O_NONBLOCK))

    @classmethod
    def connect_seqpacket(cls, path: str) -> "FdLink":
        s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        s.connect(path)
        fd = os.dup(s.fileno())
        s.close()
        return cls(fd)

    def _on_readable(self) -> None:
        while True:
            try:
                data = os.read(self.fd, self.MAX_MSG)
            except BlockingIOError:
                return
            except OSError:
                data = b""
            if not data:  # EOF or error: peer gone
                self._shutdown()
                return
            self._rx.put_nowait(data)

    def _shutdown(self) -> None:
        if not self._closed:
            self._closed = True
            try:
                self.loop.remove_reader(self.fd)
            except Exception:
                pass
            self._rx.put_nowait(_CLOSE)

    async def send(self, data: bytes) -> bool:
        if self._closed:
            raise LinkClosed()
        try:
            n = os.write(self.fd, data)
        except BlockingIOError:
            self.dropped += 1
            return False
        except OSError as e:
            if e.errno in (errno.EPIPE, errno.ECONNRESET, errno.EBADF, errno.ENOTCONN):
                self._shutdown()
                raise LinkClosed() from e
            if e.errno in (errno.ENOBUFS, errno.EAGAIN):
                self.dropped += 1
                return False
            raise
        if n != len(data):  # a message-oriented transport never splits
            self.dropped += 1
            return False
        self.sent += 1
        return True

    async def recv(self) -> bytes:
        item = await self._rx.get()
        if item is _CLOSE:
            self._rx.put_nowait(_CLOSE)
            raise LinkClosed()
        self.received += 1
        return item

    def close(self) -> None:
        self._shutdown()
        try:
            os.close(self.fd)
        except OSError:
            pass

    @property
    def closed(self) -> bool:
        return self._closed
