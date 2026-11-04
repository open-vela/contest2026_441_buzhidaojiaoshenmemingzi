"""Offline stream/admission/real HTTP tests; no microphone, model or cloud."""
import contextlib
import http.client
import io
import json
import threading
import time
import unittest
from unittest.mock import Mock, patch
from local_asr_wake import LocalWake, matches_wake
from session_server import LANServer, handler


class Tests(unittest.TestCase):
    def setUp(self):
        self.now = 0
        self.engine = Mock()
        self.engine.feed.return_value = ("", False)
        self.wake = LocalWake(self.engine, clock=lambda: self.now)
        self.pcm = bytes(16000)

    def test_complete_official_greeting_and_token_boundaries(self):
        for text in ("你好，openvela", "Hello, Open Vela!", "你好 OPEN VELA。", "你好，ＯＰＥＮＶＥＬＡ"):
            self.assertTrue(matches_wake(text), text)
        for text in ("你好", "openvela", "帮我看看前面有什么", "hello vela", "你好openvelax", "shello openvela", "你好欧盆维拉"):
            self.assertFalse(matches_wake(text), text)

    def test_partial_then_complete_only_once(self):
        stream = self.wake.start()["stream"]
        self.engine.feed.side_effect = [("你好 open", False), ("你好 openvela", False)]
        self.now = .5
        self.assertFalse(self.wake.feed(stream, 0, self.pcm)["matched"])
        self.now = 1
        self.assertTrue(self.wake.feed(stream, 1, self.pcm)["matched"])
        with self.assertRaises(ValueError): self.wake.feed(stream, 2, self.pcm)
        self.assertEqual(self.engine.feed.call_count, 2)

    def test_out_of_order_clears_stream(self):
        stream = self.wake.start()["stream"]
        with self.assertRaises(ValueError): self.wake.feed(stream, 1, self.pcm)
        self.assertIsNone(self.wake.current)
        self.engine.feed.assert_not_called()

    def test_gap_expires_instead_of_splicing_speech(self):
        stream = self.wake.start()["stream"]
        self.now = 2.01
        with self.assertRaises(TimeoutError): self.wake.feed(stream, 0, self.pcm)
        self.engine.feed.assert_not_called()
        self.assertNotEqual(stream, self.wake.start()["stream"])

    def test_busy_and_wrong_lease_do_not_replace_owner(self):
        stream = self.wake.start()["stream"]
        with self.assertRaises(BlockingIOError): self.wake.start()
        with self.assertRaises(TimeoutError): self.wake.feed("f"*32, 0, self.pcm)
        self.wake.stop("f"*32)
        self.assertEqual(self.wake.current["id"], stream)
        self.wake.stop(stream)
        self.assertIsNone(self.wake.current)

    def test_backlog_cannot_run_ahead_of_live_clock(self):
        stream = self.wake.start()["stream"]
        self.wake.feed(stream, 0, self.pcm)
        self.wake.feed(stream, 1, self.pcm)
        with self.assertRaises(ValueError): self.wake.feed(stream, 2, self.pcm)
        self.assertIsNone(self.wake.current)

    def test_slow_inference_cannot_wake_late(self):
        stream = self.wake.start()["stream"]
        def slow(*_):
            self.now += .81
            return "你好openvela", False
        self.engine.feed.side_effect = slow
        with self.assertRaises(TimeoutError): self.wake.feed(stream, 0, self.pcm)
        self.assertIsNone(self.wake.current)

    def test_failed_inference_discards_partially_decoded_state(self):
        stream = self.wake.start()["stream"]
        self.engine.feed.side_effect = RuntimeError("decode failed")
        with self.assertRaises(RuntimeError): self.wake.feed(stream, 0, self.pcm)
        self.assertIsNone(self.wake.current)

    def test_invalid_chunk_is_never_decoded(self):
        for size in (0, 640, 15999, 16002):
            stream = self.wake.start()["stream"]
            with self.assertRaises(ValueError): self.wake.feed(stream, 0, bytes(size))
        self.engine.feed.assert_not_called()

    def test_text_trace_requires_explicit_opt_in(self):
        stream = self.wake.start()["stream"]
        self.engine.feed.return_value = ("secret speech", False)
        output = io.StringIO()
        with contextlib.redirect_stdout(output): self.wake.feed(stream, 0, self.pcm)
        self.assertEqual(output.getvalue(), "")
        self.wake.trace_text = True
        with contextlib.redirect_stdout(output): self.wake.feed(stream, 1, self.pcm)
        self.assertEqual(output.getvalue(), "")  # repeated partial is not spammed
        self.engine.feed.return_value = ("new\ntext", False)
        self.now = 1
        with contextlib.redirect_stdout(output): self.wake.feed(stream, 2, self.pcm)
        self.assertIn('"text": "new text"', output.getvalue())
        self.assertNotIn(stream, output.getvalue())

    def test_real_http_auth_bounds_and_voice_gate(self):
        manager = Mock(voice_enabled=False)
        server = LANServer(("127.0.0.1", 0), handler(manager, "a"*64, wake=self.wake))
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        def post(path, body, key="a"*64, headers=None):
            conn = http.client.HTTPConnection(*server.server_address, timeout=2)
            conn.request("POST", path, body, {"Authorization": "Bearer " + key,
                         "Content-Type": "application/json", **(headers or {})})
            response = conn.getresponse()
            value = json.loads(response.read())
            status = response.status
            conn.close()
            return status, value
        try:
            self.assertEqual(post("/wake/start", b"{}", key="b"*64)[0], 401)
            self.engine.create.assert_not_called()
            status, result = post("/wake/start", b"{}")
            self.assertEqual(status, 200)
            stream = result["stream"]
            self.assertEqual(post("/wake/start", b"{}")[0], 429)
            self.assertEqual(post("/wake/frame", bytes(16001))[0], 413)
            self.assertEqual(post("/wake/frame", self.pcm, headers={"Content-Type": "application/octet-stream", "X-R1-Wake": stream+":0"})[0], 200)
            self.assertEqual(post("/session/start", b"{}")[0], 403)
            manager.start.assert_not_called()
            self.assertEqual(post("/wake/stop", json.dumps({"stream": stream}).encode())[0], 200)
            self.assertIsNone(self.wake.current)
        finally:
            server.shutdown(); thread.join(); server.server_close()

    def test_endpoint_trace_not_lost_when_final_text_is_unchanged(self):
        self.wake.trace_text = True
        stream = self.wake.start()["stream"]
        self.engine.feed.side_effect = [("你好 OPEN", False), ("你好 OPEN", True)]
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.wake.feed(stream, 0, self.pcm)
            self.wake.feed(stream, 1, self.pcm)
        records = [json.loads(line.removeprefix("R1_WAKE_TEXT ")) for line in output.getvalue().splitlines()]
        self.assertEqual([row["endpoint"] for row in records], [False, True])
        self.assertEqual([row["audio_ms"] for row in records], [500, 1000])
        self.assertTrue(all(not row["matched"] for row in records))

    def test_streaming_idle_and_decoder_reset_keep_ordered_ownership(self):
        stream = self.wake.start(streaming=True)["stream"]
        self.now = 2.2
        self.engine.feed.return_value = ("你好openvela", False)
        self.assertTrue(self.wake.feed(stream, 0, self.pcm)["matched"])
        self.wake.reset_decoder(stream)
        self.engine.feed.return_value = ("普通问候", False)
        self.assertFalse(self.wake.feed(stream, 1, self.pcm)["matched"])
        self.assertEqual(self.engine.create.call_count, 2)
        self.assertEqual(self.wake.current["seq"], 2)
        self.now = 5.21
        self.wake.reap()
        self.assertIsNone(self.wake.current)

    def test_shutdown_refuses_new_streams_and_clears_current_owner(self):
        stream = self.wake.start(streaming=True)["stream"]
        self.wake.close()
        self.assertIsNone(self.wake.current)
        with self.assertRaises(ValueError): self.wake.start(streaming=True)
        with self.assertRaises(TimeoutError): self.wake.feed(stream, 0, self.pcm)

    @contextlib.contextmanager
    def tunnel_server(self):
        manager = Mock(voice_enabled=False)
        server = LANServer(("127.0.0.1", 0), handler(manager, "a"*64, wake=self.wake))
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            yield server, manager
        finally:
            server.shutdown(); thread.join(); server.server_close()

    def connect_tunnel(self, server, key="a"*64, target=None):
        conn = http.client.HTTPConnection(*server.server_address, timeout=2)
        conn.set_tunnel(*(target or server.server_address), headers={
            "Authorization": "Bearer " + key, "X-R1-Wake-Stream": "1"})
        try:
            conn.connect()
        except Exception:
            conn.close()
            raise
        conn.sock.sendall(b"R1A2")
        reader = conn.sock.makefile("rb")
        hello = json.loads(reader.readline())
        self.assertEqual(hello["protocol"], 2)
        return conn, reader, hello["stream"]

    def test_real_connect_stream_reassembles_frames_and_never_starts_cloud(self):
        self.engine.feed.side_effect = [("你好openvela", False), ("新的句子", False)]
        with self.tunnel_server() as (server, manager):
            conn, reader, stream = self.connect_tunnel(server)
            try:
                for sequence in range(50):
                    raw = sequence.to_bytes(4, "big") + bytes(640)
                    # Actual TCP fragmentation, not an assumed recv boundary.
                    for part in (raw[:3], raw[3:21], raw[21:]): conn.sock.sendall(part)
                    if sequence in (24, 49):
                        reply = json.loads(reader.readline())
                        self.assertEqual(reply["stream"], stream)
                        self.assertEqual(reply["last_frame"], sequence)
                        self.assertEqual(reply["matched"], sequence == 24)
                self.assertEqual(self.engine.feed.call_count, 2)
                manager.start.assert_not_called()
            finally:
                reader.close(); conn.close()
            for _ in range(100):
                if self.wake.current is None: break
                time.sleep(.01)
            self.assertIsNone(self.wake.current)

    def test_connect_rejects_wrong_auth_and_forwarding_target(self):
        with self.tunnel_server() as (server, manager), \
             patch("session_server.configure_wake_stream") as configure:
            for kwargs in ({"key": "b"*64}, {"target": ("example.invalid", 443)}):
                with self.assertRaises(OSError): self.connect_tunnel(server, **kwargs)
            self.engine.create.assert_not_called()
            manager.start.assert_not_called()
            configure.assert_not_called()

    def test_connect_ack_configuration_failure_rejects_before_stream_ownership(self):
        with self.tunnel_server() as (server, manager), \
             patch("session_server.configure_wake_stream", side_effect=OSError("private-detail")) as configure:
            with self.assertRaises(OSError) as caught:
                self.connect_tunnel(server)
            self.assertNotIn("private-detail", str(caught.exception))
            configure.assert_called_once()
            self.engine.create.assert_not_called()
            manager.start.assert_not_called()
            self.assertIsNone(self.wake.current)

    def test_connect_sequence_failure_discards_stream_before_asr(self):
        with self.tunnel_server() as (server, manager):
            conn, reader, _ = self.connect_tunnel(server)
            try:
                conn.sock.sendall((1).to_bytes(4, "big") + bytes(640))
                self.assertEqual(reader.read(), b"")
                self.engine.feed.assert_not_called()
                self.assertIsNone(self.wake.current)
            finally:
                reader.close(); conn.close()


if __name__ == "__main__":
    unittest.main()
