"""Real UDP / HTTP plus deterministic loss, auth and expiry checks; no cloud."""
import contextlib
import http.client
import json
import io
import socket
import struct
import threading
import unittest
from unittest.mock import Mock
from local_asr_wake import LocalWake
from session_server import LANServer, handler
from wake_udp import WakeUDP, signed, PACKET_BYTES, ACK_BYTES


class Tests(unittest.TestCase):
    def setUp(self):
        self.now = 0
        self.engine = Mock()
        self.engine.feed.return_value = ("", False)
        self.wake = LocalWake(self.engine, clock=lambda: self.now)
        self.udp = WakeUDP(("127.0.0.1", 0), "a"*64, self.wake, clock=lambda: self.now)
        self.addCleanup(self.udp.close)
        self.stream = self.udp.start("127.0.0.1")["stream"]
        self.peer = ("127.0.0.1", 55001)

    def packet(self, sequence, *, key=None, stream=None):
        # Distinct PCM per frame makes dropped / duplicated / spliced data visible.
        previous = struct.pack("<320h", *([max(0, sequence - 1)]*320))
        current = struct.pack("<320h", *([sequence]*320))
        return signed(key or bytes.fromhex("a"*64), b"R1U1" +
                      bytes.fromhex(stream or self.stream) + struct.pack("!I", sequence) + previous + current)

    def accept(self, sequence):
        self.now = (sequence + 1) * .02
        return self.udp._accept(self.packet(sequence), self.peer)

    def decoded(self):
        return b"".join(call.args[1] for call in self.engine.feed.call_args_list)

    def test_single_loss_repaired_exactly_without_duplicate_audio(self):
        for index in range(25):
            if index != 12:
                reply = self.accept(index)
        self.assertEqual(len(self.packet(0)), PACKET_BYTES)
        self.assertEqual(len(reply), ACK_BYTES)
        self.assertEqual(reply[:4], b"R1K1")
        self.assertEqual(reply, signed(bytes.fromhex("a"*64), reply[:-16]))
        expected = b"".join(struct.pack("<320h", *([i]*320)) for i in range(25))
        self.assertEqual(self.decoded(), expected)
        self.assertEqual(self.udp.current["repaired"], 1)
        self.assertEqual(self.udp.current["gaps"], 0)
        self.assertIsNone(self.udp._accept(self.packet(12), self.peer))
        self.assertEqual(self.engine.feed.call_count, 1)

    def test_first_packet_loss_recovers_frame_zero(self):
        for index in range(1, 25):
            self.accept(index)
        self.assertEqual(len(self.decoded()), 16000)
        self.assertEqual(self.decoded()[:640], bytes(640))
        self.assertEqual(self.udp.current["repaired"], 1)

    def test_burst_loss_resets_decoder_and_discards_incomplete_chunk(self):
        for index in range(15):
            self.accept(index)
        for index in range(23, 47):
            self.accept(index)
        self.assertEqual(self.udp.current["gaps"], 1)
        self.assertEqual(self.engine.create.call_count, 2)
        values = struct.unpack("<8000h", self.decoded())
        self.assertEqual(values[::320], tuple(range(22, 47)))

    def test_short_loss_keeps_time_with_silence_instead_of_erasing_utterance(self):
        for index in range(25):
            if index not in (14, 15, 16):
                self.accept(index)
        values = struct.unpack('<8000h', self.decoded())
        expected = list(range(25))
        expected[14:16] = [0, 0]  # Frame16 recovered from the previous-frame field.
        self.assertEqual(values[::320], tuple(expected))
        self.assertEqual(self.udp.current['concealed'], 2)
        self.assertEqual(self.udp.current['short_gaps'], 1)
        self.assertEqual(self.udp.current['gaps'], 0)
        self.assertEqual(self.engine.create.call_count, 1)

    def test_ack_waits_two_frames_and_stop_discards_pending_old_owner(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.bind(('127.0.0.1', 0))
            client.settimeout(.05)
            peer = client.getsockname()
            self.udp._accept(self.packet(0), peer)
            raw = signed(bytes.fromhex('a'*64), b'R1K1'+bytes.fromhex(self.stream)+struct.pack('!IB', 0, 0))
            self.udp._schedule(raw, peer)
            self.now = .039
            self.udp._send_due()
            with self.assertRaises(socket.timeout):
                client.recv(100)
            self.now = .041
            self.udp._send_due()
            self.assertEqual(client.recv(100), raw)
            self.udp._schedule(raw, peer)
            self.udp.stop(self.stream)
            self.assertFalse(self.udp.pending)
            self.now += 1
            self.udp._send_due()
            with self.assertRaises(socket.timeout):
                client.recv(100)

    def test_auth_nonce_size_peer_and_ahead_clock_do_not_touch_decoder(self):
        valid = self.packet(0)
        wrong_tag = valid[:-1] + bytes([valid[-1] ^ 1])
        for raw, peer in ((valid[:-1], self.peer), (valid+b"x", self.peer),
                          (wrong_tag, self.peer), (self.packet(0, stream="b"*32), self.peer),
                          (self.packet(0, key=b"b"*32), self.peer),
                          (valid, ("127.0.0.2", 55001)), (self.packet(100), self.peer)):
            self.assertIsNone(self.udp._accept(raw, peer))
        self.engine.feed.assert_not_called()
        self.assertEqual(self.udp.current["expected"], 0)
        self.assertEqual(self.udp.current["pcm"], b"")
        self.accept(0)
        self.assertIsNone(self.udp._accept(self.packet(1), ("127.0.0.1", 55002)))

    def test_stale_audio_does_not_renew_expiry_or_emit_late_match(self):
        self.now = 1.6
        self.assertIsNone(self.udp._accept(self.packet(0), self.peer))
        self.assertEqual(self.udp.current["last"], 0)
        self.now = 3.01
        self.assertIsNone(self.udp._accept(self.packet(0), self.peer))
        self.assertIsNone(self.udp.current)
        self.assertIsNone(self.wake.current)
        self.engine.feed.assert_not_called()

    def test_positive_ack_repeated_for_loss_then_new_decoder(self):
        self.engine.feed.return_value = ("你好openvela", False)
        replies = [self.accept(i) for i in range(28)]
        self.assertEqual(replies[24], replies[25])
        self.assertEqual(replies[24], replies[26])
        self.assertIsNone(replies[27])
        self.assertEqual(replies[24][24], 1)
        self.assertEqual(self.engine.create.call_count, 2)
        self.engine.feed.return_value = ("普通问候", False)
        for index in range(28, 50):
            reply = self.accept(index)
        self.assertEqual(reply[24], 0)
        self.assertEqual(struct.unpack_from("!I", reply, 20)[0], 49)

    def test_stop_wrong_owner_and_shutdown_clear_ram(self):
        self.accept(0)
        c = self.udp.current
        self.udp.stop("b"*32)
        self.assertIs(self.udp.current, c)
        with self.assertRaises(BlockingIOError): self.udp.start("127.0.0.1")
        self.udp.close()
        self.assertEqual(c["pcm"], b"")
        self.assertIsNone(self.wake.current)
        self.assertFalse(self.udp.thread.is_alive())
        with self.assertRaises(ValueError): self.udp.start("127.0.0.1")

    def test_peer_icmp_after_close_does_not_disable_next_stream(self):
        # Use the real listener and receiver thread, injecting only the OS's
        # ICMP notification so Linux and Windows cover the same boundary.
        real_socket = self.udp.socket
        errors = [ConnectionResetError('peer ICMP reset'), ConnectionRefusedError('peer ICMP refused')]
        delivered = threading.Event()
        class NotifyingSocket:
            def __getattr__(self, name):
                return getattr(real_socket, name)
            def recvfrom(self, size):
                if errors:
                    error = errors.pop(0)
                    if not errors:
                        delivered.set()
                    raise error
                return real_socket.recvfrom(size)
        self.udp.stop(self.stream)
        self.udp.socket = NotifyingSocket()
        self.assertTrue(delivered.wait(1))
        self.assertFalse(self.udp.closed.is_set())
        self.assertTrue(self.udp.thread.is_alive())
        self.stream = self.udp.start('127.0.0.1')['stream']
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.settimeout(2)
            for index in range(25):
                client.sendto(self.packet(index), self.udp.address)
            import time
            for _ in range(100):
                with self.udp.lock:
                    scheduled = bool(self.udp.pending)
                if scheduled:
                    break
                time.sleep(.005)
            self.assertTrue(scheduled)
            self.now += .05
            reply = client.recv(100)
            self.assertEqual(reply[4:20], bytes.fromhex(self.stream))
            self.assertEqual(reply[24], 0)

    def test_real_http_auth_udp_transfer_stop_and_voice_gate(self):
        self.udp.stop(self.stream)
        manager = Mock(voice_enabled=False)
        server = LANServer(("127.0.0.1", 0), handler(manager, "a"*64, wake=self.wake, wake_udp=self.udp))
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        def post(path, body, key="a"*64):
            with contextlib.closing(http.client.HTTPConnection(*server.server_address, timeout=2)) as conn:
                conn.request("POST", path, json.dumps(body), {"Authorization": "Bearer " + key})
                response = conn.getresponse()
                return response.status, json.loads(response.read())
        try:
            self.assertEqual(post("/wake/udp-start", {}, key="b"*64)[0], 401)
            self.assertIsNone(self.wake.current)
            status, hello = post("/wake/udp-start", {})
            self.assertEqual(status, 200)
            self.stream = hello["stream"]
            self.assertEqual(hello["protocol"], 3)
            self.assertEqual(hello["udp_port"], self.udp.address[1])
            self.assertEqual(post("/wake/start", {})[0], 429)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.settimeout(2)
                for index in range(25):
                    client.sendto(self.packet(index), self.udp.address)
                # The test clock is explicit; advance it after the receiver
                # has scheduled the deferred ACK, including the no-more-PCM
                # timeout path used by a client ending a chunk here.
                import time
                for _ in range(100):
                    with self.udp.lock:
                        scheduled = bool(self.udp.pending)
                    if scheduled:
                        break
                    time.sleep(.005)
                self.assertTrue(scheduled)
                self.now += .05
                reply = client.recv(100)
                self.assertEqual(len(reply), ACK_BYTES)
                self.assertEqual(struct.unpack_from("!I", reply, 20)[0], 24)
            self.assertEqual(post("/session/start", {})[0], 403)
            manager.start.assert_not_called()
            self.assertEqual(post("/wake/stop", {"stream": self.stream})[0], 200)
            self.assertIsNone(self.wake.current)
            self.assertIsNone(self.udp.current)
        finally:
            server.shutdown()
            thread.join()
            server.server_close()

    def test_failed_decoder_is_cleared_by_receiver_and_can_restart(self):
        self.engine.feed.side_effect = RuntimeError("decode failure")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            for index in range(25):
                client.sendto(self.packet(index), self.udp.address)
        # Acquiring the receiver lock establishes that its clear completed.
        import time
        for _ in range(100):
            with self.udp.lock:
                if self.udp.current is None:
                    break
            time.sleep(.01)
        self.assertIsNone(self.udp.current)
        self.assertIsNone(self.wake.current)
        self.assertTrue(self.udp.thread.is_alive())

    def test_slow_inference_rejects_match_logs_fixed_reason_and_allows_fresh_owner(self):
        self.udp.trace = True
        def slow(*args):
            self.now += .801
            return '你好openvela', False
        self.engine.feed.side_effect = slow
        old_owner = self.udp.current
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.settimeout(.08)
                import time
                # Do not overflow the intentionally bounded kernel receive
                # buffer with a synthetic33KiB instantaneous burst.
                for index in range(25):
                    client.sendto(self.packet(index), self.udp.address)
                    time.sleep(.002)
                for _ in range(100):
                    with self.udp.lock:
                        cleared = self.udp.current is None
                    if cleared: break
                    time.sleep(.005)
                self.assertTrue(cleared)
                with self.assertRaises(socket.timeout): client.recv(100)
        lines = [json.loads(line.removeprefix('R1_WAKE_UDP '))
                 for line in output.getvalue().splitlines() if line.startswith('R1_WAKE_UDP ')]
        self.assertEqual(len(lines), 1)
        self.assertEqual(lines[0]['reason'], 'inference-budget')
        self.assertEqual(lines[0]['frames'], 25)
        for secret in (self.stream, '你好openvela', 'a'*64, self.peer[0]):
            self.assertNotIn(secret, output.getvalue())
        self.assertIsNone(self.wake.current)
        self.assertEqual(old_owner['pcm'], b'')
        self.assertFalse(self.udp.pending)
        self.assertTrue(self.udp.thread.is_alive())
        self.engine.feed.side_effect = None
        old_stream = self.stream
        self.stream = self.udp.start('127.0.0.1')['stream']
        self.assertNotEqual(self.stream, old_stream)
        self.assertIsNone(self.udp._accept(self.packet(0, stream=old_stream), self.peer))
        base = self.now
        for index in range(25):
            self.now = base + (index+1)*.02
            reply = self.udp._accept(self.packet(index), self.peer)
        self.assertEqual(reply[4:20], bytes.fromhex(self.stream))
        self.assertEqual(reply[24], 0)
        self.assertEqual(self.engine.feed.call_count, 2)
        self.assertEqual(self.udp.current['pcm'], b'')


if __name__ == "__main__":
    unittest.main()
