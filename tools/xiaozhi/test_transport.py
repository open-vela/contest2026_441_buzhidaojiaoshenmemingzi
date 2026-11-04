#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real libwebsockets client against a silent local mock, no microphone/API."""
import asyncio
import json
import sys
import websockets


async def main():
    phase = 0

    async def handler(ws):
        nonlocal phase
        current = phase
        phase += 1
        assert ws.request.path == "/xiaozhi/v1/"
        assert ws.request.headers["Device-Id"] == "r1-silent-test"
        assert ws.request.headers["Authorization"] == "Bearer test-only"
        hello = json.loads(await ws.recv())
        assert hello["audio_params"]["frame_duration"] == 60
        if current == 2:
            await ws.wait_closed()
            return
        if current == 1:
            try:
                await ws.send("x" * 9000)
                await ws.wait_closed()
            except websockets.ConnectionClosed:
                pass
            return
        greeting = json.dumps({"type": "hello", "transport": "websocket",
                               "session_id": "silent-session"})
        await ws.send([greeting[:10], greeting[10:]])
        listen = json.loads(await ws.recv())
        assert listen["type"] == "listen" and listen["state"] == "start"
        await ws.send(json.dumps({"type": "tts", "state": "start"}))
        for index in range(64):
            packet = bytes([index]) * 301
            await ws.send([packet[:7], packet[7:201], packet[201:]])
        pong = await ws.ping(b"keepalive")
        await asyncio.wait_for(pong, 10)
        await ws.send(json.dumps({"type": "tts", "state": "stop"}))
        await ws.wait_closed()

    async with websockets.serve(handler, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        proc = await asyncio.create_subprocess_exec(sys.argv[1], str(port))
        try:
            code = await asyncio.wait_for(proc.wait(), 25)
            assert code == 0 and phase == 3, (code, phase)
        finally:
            if proc.returncode is None:
                proc.kill()
                await proc.wait()


if __name__ == "__main__":
    asyncio.run(main())
