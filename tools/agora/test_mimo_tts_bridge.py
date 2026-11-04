"""Offline protocol, truncation, authentication and free-only policy tests."""
import base64
from contextlib import redirect_stdout
from datetime import datetime, timezone
import http.client
import io
import json
import threading
import time
import unittest
from unittest.mock import patch
import urllib.error

import mimo_tts_bridge as tts
import session_control as control


def config():
    return {"api_key": "sk-" + "test-key-not-real" * 2, "model": tts.MODEL,
            "voice": "冰糖", "free_only": True,
            "free_verified_until": datetime.fromtimestamp(time.time() + 3600, timezone.utc).isoformat(),
            "max_requests": 20, "public_url": "https://example.invalid/v1/audio/speech",
            "agora_free_minutes": {"rtc": 10000, "agent": 985, "asr": 990}}


def event(raw):
    obj = {"choices": [{"delta": {"audio": {"data": base64.b64encode(raw).decode()}}}]}
    return b"data: " + json.dumps(obj).encode() + b"\n\n"


class Tests(unittest.TestCase):
    def parse(self, raw):
        return b"".join(tts.read_pcm(io.BytesIO(raw), time.monotonic()))

    def test_sse_pcm_alignment_across_events(self):
        self.assertEqual(self.parse(b": heartbeat\n\n" + event(b"\x01") + event(b"\x02\x03\x04") +
                                   b"data: {\"choices\": []}\n\ndata: [DONE]\n\n"), b"\x01\x02\x03\x04")

    def test_empty_truncated_invalid_stream(self):
        for raw in (b"data: [DONE]\n\n", event(b"\x01\x02"), event(b"\x01") + b"data: [DONE]\n\n",
                    b"data: {\"error\": \"secret\"}\n\n", b"data: {\"choices\": [4]}\n\n",
                    b"data: {\"choices\": [{\"delta\": {\"audio\": {\"data\": \"!!!!\"}}}]}\n\n",
                    b"data: " + b"x" * 262145 + b"\n\n", b"data: not-json\n\n"):
            with self.subTest(raw_len=len(raw)), self.assertRaises(ValueError): self.parse(raw)

    def test_time_and_size_limits(self):
        with self.assertRaises(TimeoutError):
            list(tts.read_pcm(io.BytesIO(event(b"aa")), time.monotonic() - 31))
        with patch.object(tts, "MAX_PCM", 2), self.assertRaises(ValueError):
            self.parse(event(b"aaaa") + b"data: [DONE]\n\n")

    def test_price_key_model_gates(self):
        cfg = config()
        tts.validate_config(cfg)
        for key, bad in (("api_key", "tp-" + "a" * 40), ("free_only", False),
                         ("model", "paid-fallback"), ("voice", "not-a-voice"),
                         ("free_verified_until", "2000-01-01T00:00:00+00:00"),
                         ("free_verified_until", "2099-01-01T00:00:00+00:00"),
                         ("free_verified_until", "2026-10-11T00:00:00"),
                         ("max_requests", True), ("max_requests", 21)):
            with self.subTest(key=key, value=bad), self.assertRaises(ValueError):
                tts.validate_config({**cfg, key: bad})

    def test_input_format_is_not_guessed(self):
        cfg = config()
        self.assertEqual(tts.validate_input({"input": "你好"}, cfg), ("你好", ""))
        for body in ([], {}, {"input": " "}, {"input": "a" * 501},
                     {"input": "hi", "sample_rate": 16000}, {"input": "hi", "sample_rate": 24000.0},
                     {"input": "hi", "response_format": "wav"}, {"input": "hi", "speed": True},
                     {"input": "hi", "model": "paid"}, {"input": "hi", "instruction": 1}):
            with self.assertRaises(ValueError): tts.validate_input(body, cfg)

    def test_request_budget_and_busy(self):
        bridge = tts.Bridge({**config(), "max_requests": 1})
        self.assertTrue(bridge.acquire())
        self.assertFalse(bridge.acquire())
        bridge.lock.release()
        with self.assertRaises(ValueError): bridge.acquire()
        self.assertFalse(bridge.lock.locked())

    def test_real_upstream_request_contract_without_network(self):
        response = io.BytesIO(event(b"abcd") + b"data: [DONE]\n\n")
        response.headers = {"Content-Type": "text/event-stream"}
        class Opener:
            def open(self, req, timeout):
                self.req, self.timeout = req, timeout
                return response
        opener = Opener()
        with patch("mimo_tts_bridge.urllib.request.build_opener", return_value=opener):
            self.assertEqual(b"".join(tts.synthesize(config(), "你好", "温和")), b"abcd")
        body = json.loads(opener.req.data)
        self.assertEqual(opener.req.full_url, tts.ENDPOINT)
        self.assertEqual(body["model"], "mimo-v2.5-tts")
        self.assertIs(body["stream"], True)
        self.assertEqual(body["messages"][-1], {"role": "assistant", "content": "你好"})
        self.assertEqual(body["audio"]["format"], "pcm16")
        self.assertEqual(opener.timeout, 10)

    def test_upstream_auth_error_is_sanitized_and_not_retried(self):
        error = urllib.error.HTTPError(tts.ENDPOINT, 401, "secret-body", {}, io.BytesIO(b"secret-body"))
        with patch("mimo_tts_bridge.urllib.request.build_opener") as opener:
            opener.return_value.open.side_effect = error
            with self.assertRaisesRegex(RuntimeError, "HTTP 401; no fallback") as caught:
                b"".join(tts.synthesize(config(), "你好"))
            self.assertNotIn("secret", str(caught.exception))
            self.assertEqual(opener.return_value.open.call_count, 1)
        with self.assertRaises(ValueError): tts.NoRedirect().redirect_request(None)

    def test_cloud_config_has_no_paid_inheritance(self):
        cfg = config()
        with patch.object(tts, "load_config", return_value=cfg), patch.object(tts, "bridge_key", return_value="b" * 64):
            voice = tts.agora_voice_config()
        self.assertEqual(voice["tts"]["vendor"], "generic_http")
        self.assertEqual(voice["tts"]["params"]["sample_rate"], 24000)
        self.assertEqual(voice["asr"]["vendor"], "fengming")
        self.assertEqual(voice["turn_detection"]["config"]["end_of_speech"]["mode"], "vad")
        self.assertNotIn("semantic", json.dumps(voice))
        self.assertNotIn(cfg["api_key"], json.dumps(voice))
        for url in ("", "http://example.invalid/v1/audio/speech", "https://secret@example.invalid/v1/audio/speech",
                    "https://example.invalid/wrong", "https://example.invalid/v1/audio/speech?key=x"):
            with patch.object(tts, "load_config", return_value={**cfg, "public_url": url}), self.assertRaises(ValueError):
                tts.agora_voice_config()
        with patch.object(tts, "load_config", return_value={**cfg, "agora_free_minutes": {}}), self.assertRaises(ValueError):
            tts.agora_voice_config()

    def test_paid_llm_is_blocked(self):
        with patch.object(control, "llm_config", return_value={"url": "https://example.invalid", "params": {"model": "paid"}}), \
             self.assertRaises(ValueError):
            control.free_voice_config()

    def test_free_llm_and_tts_are_explicit(self):
        llm = {"url": "https://open.bigmodel.cn/api/paas/v4/chat/completions",
               "params": {"model": "glm-4-flash-250414"}}
        with patch.object(control, "llm_config", return_value=llm), \
             patch.object(tts, "agora_voice_config", return_value={"tts": {"vendor": "generic_http"}}):
            self.assertEqual(control.free_voice_config(), {"llm": llm, "tts": {"vendor": "generic_http"}})

    def test_probe_gate_precedes_account_or_config_writes(self):
        with patch.object(control, "free_voice_config", side_effect=ValueError("gate closed")), \
             patch.object(control, "account") as account, \
             patch("sys.argv", ["session_control.py", "probe"]), self.assertRaises(ValueError):
            control.main()
        account.assert_not_called()

    def test_402_classification_does_not_leak_provider_body(self):
        body = json.dumps({"error": {"code": "provider-secret", "message": "Insufficient account balance"}}).encode()
        error = urllib.error.HTTPError(tts.ENDPOINT, 402, "payment", {}, io.BytesIO(body))
        with patch("mimo_tts_bridge.urllib.request.build_opener") as opener:
            opener.return_value.open.side_effect = error
            with self.assertRaisesRegex(RuntimeError, "reason=insufficient_balance_or_quota") as caught:
                b"".join(tts.synthesize(config(), "你好"))
            self.assertNotIn("provider-secret", str(caught.exception))
            self.assertEqual(opener.return_value.open.call_count, 1)

    def serve(self, source):
        bridge = tts.Bridge(config(), source)
        server = tts.Server(("127.0.0.1", 0), tts.handler(bridge, "b" * 64))
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        self.addCleanup(server.server_close)
        self.addCleanup(worker.join)
        self.addCleanup(server.shutdown)
        return server, bridge

    def post(self, server, body, auth="Bearer " + "b" * 64, path="/v1/audio/speech", headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
        self.addCleanup(connection.close)
        fields = {"Authorization": auth, "Content-Type": "application/json", **(headers or {})}
        connection.request("POST", path, json.dumps(body).encode(), fields)
        return connection.getresponse()

    def test_http_chunked_pcm_and_rejections(self):
        calls = []
        def source(cfg, text, instruction):
            calls.append(text)
            yield b"ab"
            yield b"cd"
        server, bridge = self.serve(source)
        for body, auth, path, status in (({"input": "hi"}, "wrong", "/v1/audio/speech", 401),
                                        ({"input": "hi"}, "Bearer " + "b" * 64, "/wrong", 404),
                                        ({"input": " "}, "Bearer " + "b" * 64, "/v1/audio/speech", 400)):
            response = self.post(server, body, auth, path)
            self.assertEqual(response.status, status)
            response.read()
        self.assertFalse(calls)
        with redirect_stdout(io.StringIO()):
            response = self.post(server, {"input": "hi", "sample_rate": 24000})
            self.assertEqual(response.status, 200)
            self.assertEqual(response.getheader("Transfer-Encoding"), "chunked")
            self.assertEqual(response.getheader("X-Audio-Sample-Rate"), "24000")
            self.assertEqual(response.read(), b"abcd")
        self.assertEqual(calls, ["hi"])
        # Reading the final HTTP chunk can precede the worker's finally.
        # Verify bounded release without racing that independent thread.
        self.assertTrue(bridge.lock.acquire(timeout=1))
        bridge.lock.release()

    def test_http_upstream_failure_and_partial_truncation(self):
        def source(cfg, text, instruction):
            if text == "partial": yield b"ab"
            raise RuntimeError("secret-provider-body")
        server, bridge = self.serve(source)
        with redirect_stdout(io.StringIO()) as captured:
            response = self.post(server, {"input": "early"})
            self.assertEqual(response.status, 502)
            self.assertNotIn(b"secret", response.read())
            response = self.post(server, {"input": "partial"})
            self.assertEqual(response.status, 200)
            with self.assertRaises(http.client.IncompleteRead) as error: response.read()
            self.assertEqual(error.exception.partial, b"ab")
        self.assertNotIn("secret", captured.getvalue())
        self.assertFalse(bridge.lock.locked())


if __name__ == "__main__": unittest.main()
