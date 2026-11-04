# SPDX-License-Identifier: Apache-2.0

import asyncio
import json
import unittest

from websockets.asyncio.server import serve

from probe import hello, probe, validate_config, validate_hello


def reply(**params):
    return json.dumps({"type": "hello", "transport": "websocket",
                       "session_id": "test-session",
                       "audio_params": {**hello()["audio_params"], **params}})


class ContractTests(unittest.TestCase):
    def test_client_does_not_claim_aec(self):
        self.assertNotIn("features", hello())

    def test_supported_rates(self):
        for rate in (16000, 24000):
            self.assertEqual(validate_hello(reply(sample_rate=rate))["audio_params"]["sample_rate"], rate)

    def test_optional_audio_params(self):
        self.assertEqual(validate_hello(json.dumps({"type": "hello", "transport": "websocket",
                                                   "session_id": "test"}))["audio_params"],
                         hello()["audio_params"])

    def test_reject_unsupported_geometry(self):
        for fields in ({"format": "pcm"}, {"channels": 2}, {"channels": True},
                       {"sample_rate": 48000}, {"frame_duration": 20}):
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                validate_hello(reply(**fields))

    def test_invalid_hello(self):
        for payload in (b"opus", "[]", "null", "{", '{"type":"stt"}',
                        '{"type":"hello","transport":"websocket"}'):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                validate_hello(payload)

    def test_header_injection(self):
        with self.assertRaises(ValueError):
            validate_config({"url": "ws://localhost/", "device_id": "02:00:00:00:04:41",
                             "client_id": "probe", "token": "x\r\nInjected: y"})


class NetworkTests(unittest.IsolatedAsyncioTestCase):
    async def test_silent_handshake_and_close(self):
        observed = []

        async def handler(websocket):
            self.assertEqual(websocket.request.headers["Protocol-Version"], "1")
            self.assertEqual(websocket.request.headers["Device-Id"], "02:00:00:00:04:41")
            self.assertEqual(websocket.request.headers["Client-Id"], "host-test")
            self.assertNotIn("Authorization", websocket.request.headers)
            observed.append(json.loads(await websocket.recv()))
            await websocket.send(reply(sample_rate=24000))
            await websocket.wait_closed()

        async with serve(handler, "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            result = await probe({"url": f"ws://127.0.0.1:{port}/xiaozhi/v1/",
                                  "device_id": "02:00:00:00:04:41",
                                  "client_id": "host-test", "timeout_seconds": 1})
            self.assertEqual(result["audio_params"]["sample_rate"], 24000)
        self.assertEqual(observed, [hello()])  # No listen, binary or model request.

    async def test_missing_hello_times_out(self):
        async def handler(websocket):
            await websocket.recv()
            await websocket.wait_closed()

        async with serve(handler, "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            with self.assertRaises(asyncio.TimeoutError):
                await probe({"url": f"ws://127.0.0.1:{port}/", "device_id": "02:00:00:00:04:41",
                             "client_id": "host-test", "timeout_seconds": 0.05})


if __name__ == "__main__":
    unittest.main()
