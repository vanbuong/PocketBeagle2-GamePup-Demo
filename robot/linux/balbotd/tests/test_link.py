# SPDX-License-Identifier: GPL-2.0-only
import asyncio
import os
import socket

import pytest

from balbotd.link import FdLink, LinkClosed, MemoryLink


async def test_memory_link_preserves_messages_and_counts():
    a, b = MemoryLink.pair()
    for m in (b"one", b"", b"three"):
        assert await a.send(m)
    assert [await b.recv() for _ in range(3)] == [b"one", b"", b"three"]
    assert a.sent == 3 and b.received == 3


async def test_memory_link_fault_injection():
    a, b = MemoryLink.pair()
    a.drop_filter = lambda d: d == b"drop"
    a.mutate = lambda d: d.upper()
    assert not await a.send(b"drop")
    assert await a.send(b"keep")
    assert await b.recv() == b"KEEP" and a.dropped == 1


async def test_memory_link_close_wakes_both_ends_and_stays_closed():
    a, b = MemoryLink.pair()
    waiter = asyncio.create_task(b.recv())
    await asyncio.sleep(0)
    a.close()
    with pytest.raises(LinkClosed):
        await waiter
    with pytest.raises(LinkClosed):
        await b.recv()  # still closed on later calls
    with pytest.raises(LinkClosed):
        await a.send(b"x")
    assert a.closed and b.closed is False  # peer end is told via recv, not marked closed
    with pytest.raises(LinkClosed):
        await b.send(b"x")


async def test_fd_link_over_seqpacket_keeps_message_boundaries():
    s1, s2 = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    a, b = FdLink(os.dup(s1.fileno())), FdLink(os.dup(s2.fileno()))
    s1.close()
    s2.close()
    try:
        msgs = [b"A" * 10, b"B" * 100, b"C"]
        for m in msgs:
            assert await a.send(m)
        assert [await asyncio.wait_for(b.recv(), 1) for _ in msgs] == msgs  # three reads, three messages
    finally:
        a.close()
        b.close()


async def test_fd_link_detects_peer_close():
    s1, s2 = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    a = FdLink(os.dup(s1.fileno()))
    s1.close()
    s2.close()  # peer gone
    with pytest.raises(LinkClosed):
        await asyncio.wait_for(a.recv(), 1)
    assert a.closed
    with pytest.raises(LinkClosed):
        await a.send(b"x")
    a.close()


async def test_fd_link_send_never_blocks_when_the_transport_is_full():
    s1, s2 = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    a = FdLink(os.dup(s1.fileno()))
    s1.close()
    try:
        results = [await a.send(b"x" * 400) for _ in range(5000)]  # nobody reads s2
        assert False in results, "expected the socket buffer to fill"
        assert a.dropped == results.count(False) and a.sent == results.count(True)
    finally:
        a.close()
        s2.close()
