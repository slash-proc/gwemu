#!/usr/bin/env python3
"""Transparent TCP tap for QEMU's GDB Remote Serial Protocol stub.

Sits between gnw-web-builder's backend bridge and QEMU's real gdbserver,
logging every byte in both directions so a real browser session's traffic
can be inspected instead of a synthetic test.

Usage:
    Start QEMU on an alternate port (not the port the backend expects):
        ./build/qemu-system-arm ... -gdb tcp::12340 -S

    Run this listening on the port the backend actually connects to
    (QEMU_DEFAULT_PORT in backend/src/server.ts, default 1234):
        python3 scripts/gdb_tap.py --listen-port 1234 --target-port 12340

    Then drive the app as usual -- its traffic now flows through here.
"""
import argparse
import asyncio
import datetime
import re
import socket as _socket


def fmt(direction: str, data: bytes) -> str:
    ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    try:
        text = data.decode("latin1")
    except Exception:
        text = repr(data)
    # Collapse the common, high-volume m/M memory packets' hex payload so the
    # log stays readable -- keep the command/address/length, elide the bytes.
    text = re.sub(r"(\$[mM][0-9a-fA-F]+,[0-9a-fA-F]+:?)[0-9a-fA-F]{40,}(#..)", r"\1<elided>\2", text)
    return f"[{ts}] {direction:>4} ({len(data):4d}B) {text}"


def _quickack(sock):
    # Linux resets TCP_QUICKACK to delayed-ack mode again as soon as it fires
    # once, so this needs re-arming after every single read, not just at
    # connect time (unlike TCP_NODELAY, which is a persistent mode).
    if sock is not None and hasattr(_socket, "TCP_QUICKACK"):
        try:
            sock.setsockopt(_socket.IPPROTO_TCP, _socket.TCP_QUICKACK, 1)
        except OSError:
            pass


async def pump(reader: asyncio.StreamReader, writer: asyncio.StreamWriter, label: str,
                own_sock=None):
    try:
        while True:
            data = await reader.read(65536)
            _quickack(own_sock)
            if not data:
                break
            print(fmt(label, data), flush=True)
            writer.write(data)
            await writer.drain()
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        writer.close()


def _set_nodelay(writer: asyncio.StreamWriter):
    # Tiny ping-pong RSP packets otherwise collide with the peer's ~40ms
    # delayed-ACK timer under Nagle's algorithm -- see backend/src/server.ts's
    # matching sock.setNoDelay(true) comment in gnw-web-builder for the
    # measured symptom (every request locked to a ~42ms cadence). NODELAY
    # alone only fixes stalls this tap's own sends would otherwise cause;
    # when the OTHER end (e.g. gnwmanager's plain Python socket, which this
    # project can't modify) has Nagle enabled without NODELAY, the stall
    # instead comes from THIS tap's receiving side delaying its TCP ACK --
    # see _quickack() above, called after every read to cover that direction.
    sock = writer.get_extra_info("socket")
    if sock is not None:
        sock.setsockopt(_socket.IPPROTO_TCP, _socket.TCP_NODELAY, 1)
    return sock


# QEMU's gdbstub only ever serves one attached client at a time. Without
# this, a long-lived browser tab's idle background liveness poll silently
# occupies that one slot forever, and a one-shot CLI tool (gnwmanager)
# connecting later just hangs contending for it instead of getting a clear
# error -- or, worse, a real but confusing ~5-40s timeout depending on what
# the existing session happens to be doing. Since only one of these can
# actually be useful at a time anyway, the newest connection always wins.
_active_session = None


async def handle_client(client_reader, client_writer, target_host, target_port):
    global _active_session
    peer = client_writer.get_extra_info("peername")

    old = _active_session
    if old is not None:
        print(f"=== preempting previous client to serve {peer} ===", flush=True)
        for w in (old["client_writer"], old["target_writer"]):
            try:
                w.close()
            except Exception:
                pass

    print(f"=== client connected: {peer} ===", flush=True)
    client_sock = _set_nodelay(client_writer)
    try:
        target_reader, target_writer = await asyncio.open_connection(target_host, target_port)
        target_sock = _set_nodelay(target_writer)
    except OSError as e:
        print(f"!!! could not connect to QEMU at {target_host}:{target_port}: {e}", flush=True)
        client_writer.close()
        return

    session = {"client_writer": client_writer, "target_writer": target_writer}
    _active_session = session

    await asyncio.gather(
        pump(client_reader, target_writer, "C->Q", own_sock=client_sock),
        pump(target_reader, client_writer, "Q->C", own_sock=target_sock),
    )
    if _active_session is session:
        _active_session = None
    print(f"=== client disconnected: {peer} ===", flush=True)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen-host", default="127.0.0.1")
    ap.add_argument("--listen-port", type=int, default=1234)
    ap.add_argument("--target-host", default="127.0.0.1")
    ap.add_argument("--target-port", type=int, default=12340)
    args = ap.parse_args()

    server = await asyncio.start_server(
        lambda r, w: handle_client(r, w, args.target_host, args.target_port),
        args.listen_host,
        args.listen_port,
    )
    print(f"tap listening on {args.listen_host}:{args.listen_port} -> "
          f"{args.target_host}:{args.target_port}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
