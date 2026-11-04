"""Offline lifecycle and real local HTTP tests; never calls a paid cloud API."""
from contextlib import redirect_stdout, redirect_stderr
import http.client
import io
import json
import socket
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch
import urllib.error
import urllib.request
from session_server import Manager, SceneHandoff, LANServer, handler, save_device_config, main, log_session_event


class Cloud:
    def __init__(self):
        self.calls = []
        self.fail_join = self.fail_leave = False
    def __call__(self, cfg, path, method="GET", body=None):
        self.calls.append(path)
        if path == "/join":
            if self.fail_join: raise RuntimeError("join failure")
            assert body["properties"]["remote_rtc_uids"] == ["1"]
            assert "pipeline_id" not in body
            assert body["properties"]["tts"]["vendor"] == "generic_http"
            assert body["properties"]["turn_detection"]["config"]["end_of_speech"]["mode"] == "vad"
            assert body["properties"]["parameters"]["silence_config"] == {"timeout_ms": 0}
            assert body["properties"]["parameters"]["output_audio_codec"] == "G722"
            assert "filler_words" not in body["properties"]  # Ordinary voice mode is unchanged.
            return {"agent_id": "test_agent"}
        if self.fail_leave: raise RuntimeError("HTTP 502")
        return {}


class Tests(unittest.TestCase):
    def setUp(self):
        self.cloud = Cloud()
        self.manager = Manager({"AGORA_APP_ID": "a" * 32}, "test_pipeline", self.cloud,
                               lambda *args: "test_rtc_token", lambda: {"url": "https://example.invalid"},
                               voice=lambda: {"tts": {"vendor": "generic_http"},
                                  "turn_detection": {"config": {"end_of_speech": {"mode": "vad"}}}})
    def session_events(self, output):
        return [json.loads(line.removeprefix("R1_SESSION "))
                for line in output.getvalue().splitlines() if line.startswith("R1_SESSION ")]
    def test_session_correlation_survives_stop_without_credentials(self):
        output = io.StringIO()
        with redirect_stdout(output):
            session = self.manager.start()
            self.manager.stop(session["lease"])
        events = self.session_events(output)
        self.assertEqual([event["event"] for event in events], ["joined", "leave_acknowledged"])
        self.assertEqual({event["agent_id"] for event in events}, {"test_agent"})
        self.assertTrue(all(type(event["unix_ms"]) is int for event in events))
        self.assertNotIn(session["lease"], output.getvalue())
        self.assertNotIn("test_rtc_token", output.getvalue())
        self.assertNotIn("r1_", output.getvalue())
        self.assertFalse(self.manager.sessions)
    def test_session_correlation_before_second_token_failure(self):
        output = io.StringIO()
        self.manager.mint = Mock(
            side_effect=["test_rtc_token", RuntimeError("device token failed")])
        with redirect_stdout(output), self.assertRaises(RuntimeError):
            self.manager.start()
        self.assertEqual([event["event"] for event in self.session_events(output)],
                         ["joined", "leave_acknowledged"])
        self.assertEqual(self.cloud.calls.count("/join"), 1)
        self.assertEqual(self.cloud.calls.count("/agents/test_agent/leave"), 1)
        self.assertFalse(self.manager.sessions)
    def test_session_correlation_does_not_hide_cleanup_failure(self):
        output = io.StringIO()
        with redirect_stdout(output):
            session = self.manager.start()
            self.cloud.fail_leave = True
            with self.assertRaises(RuntimeError): self.manager.stop(session["lease"])
        self.assertEqual([event["event"] for event in self.session_events(output)],
                         ["joined", "leave_failed"])
        self.assertIn(session["lease"], self.manager.sessions)
        self.cloud.fail_leave = False
        with redirect_stdout(output): self.manager.stop(session["lease"])
        self.assertEqual(self.session_events(output)[-1]["event"], "leave_acknowledged")
    def test_session_correlation_rejects_log_injection(self):
        output = io.StringIO()
        with redirect_stdout(output): log_session_event("joined", "bad\nAuthorization: token")
        self.assertEqual(len(output.getvalue().splitlines()), 1)
        self.assertEqual(self.session_events(output)[0]["agent_id"], "invalid")
        self.assertNotIn("Authorization", output.getvalue())
    def test_free_gate_fails_before_join(self):
        self.manager.voice = lambda: (_ for _ in ()).throw(ValueError("free gate closed"))
        with self.assertRaises(ValueError): self.manager.start()
        self.assertFalse(self.cloud.calls)
        self.assertFalse(self.manager.sessions)
    def test_image_only_rejects_voice_before_provider_token_or_cloud(self):
        voice, mint, api = Mock(), Mock(), Mock()
        manager = Manager({}, None, api=api, mint=mint, voice=voice, voice_enabled=False)
        with self.assertRaises(ValueError): manager.start()
        voice.assert_not_called(); mint.assert_not_called(); api.assert_not_called()
        self.assertFalse(manager.sessions)
        self.assertEqual(manager.starts, 0)
        with self.assertRaises(ValueError):
            Manager({}, "pipeline", allow_paid_pipeline=True, voice_enabled=False)
    def test_image_only_http_blocks_voice_but_keeps_scene_endpoint(self):
        manager = Manager({}, None, api=Mock(), mint=Mock(), voice=Mock(), voice_enabled=False)
        scene = Mock()
        scene.analyze.return_value = {"text": "桌面上有物体", "navigation": False}
        server = LANServer(("127.0.0.1", 0), handler(manager, "b" * 64, scene))
        worker = threading.Thread(target=server.serve_forever); worker.start()
        connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
        try:
            connection.request("POST", "/session/start", b"{}",
                               {"Authorization": "Bearer " + "b" * 64})
            response = connection.getresponse()
            self.assertEqual(response.status, 403); response.read()
            connection.close()
            connection.request("POST", "/vision/frame", b"\xff\xd8\xff\xd9",
                               {"Authorization": "Bearer " + "b" * 64,
                                "Content-Type": "image/jpeg"})
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(json.loads(response.read())["text"], "桌面上有物体")
            manager.api.assert_not_called(); manager.mint.assert_not_called()
            manager.voice.assert_not_called()
            self.assertFalse(manager.sessions)
        finally:
            connection.close(); server.shutdown(); server.server_close(); worker.join(5)
    def test_conflicting_cli_modes_fail_before_account_or_network(self):
        with patch("sys.argv", ["session_server.py", "--disable-voice", "--allow-paid-pipeline"]), \
             patch("session_server.control.account") as account, \
             redirect_stdout(io.StringIO()), patch("sys.stderr", io.StringIO()), \
             self.assertRaises(SystemExit):
            main()
        account.assert_not_called()
    def test_bounded_test_sessions(self):
        for _ in range(3):
            session = self.manager.start()
            item = self.manager.sessions[session["lease"]]
            self.assertLessEqual(item["end"] - __import__("time").monotonic(), 180)
            self.manager.stop(session["lease"])
        with self.assertRaises(RuntimeError): self.manager.start()
        self.assertEqual(self.cloud.calls.count("/join"), 3)
    def test_lifecycle(self):
        session = self.manager.start()
        self.assertNotIn("AGORA_APP_CERTIFICATE", session)
        self.assertEqual(session["uid"], 1)
        self.assertEqual(session["volume"], 2)
        with self.assertRaises(RuntimeError): self.manager.start()
        self.assertTrue(self.manager.keepalive(session["lease"])["ok"])
        self.assertTrue(self.manager.stop(session["lease"])["ok"])
        self.manager.stop(session["lease"])
        self.assertEqual(len(self.cloud.calls), 2)
        self.manager.start()
        self.manager.reap(all_sessions=True)
        self.assertFalse(self.manager.sessions)
    def test_session_deadline_starts_at_join_not_controller_launch(self):
        self.manager.session_seconds = 30
        with patch("session_server.time.monotonic", return_value=1234):
            session = self.manager.start()
        self.assertEqual(self.manager.sessions[session["lease"]]["end"], 1264)
        self.assertEqual(self.cloud.calls.count("/join"), 1)
        self.manager.stop(session["lease"])
    def test_join_delay_consumes_session_budget(self):
        self.manager.session_seconds = 30
        with patch("session_server.time.monotonic", side_effect=[100, 120]):
            session = self.manager.start()
        self.assertEqual(self.manager.sessions[session["lease"]]["end"], 130)
        self.manager.stop(session["lease"])
    def test_session_duration_validation(self):
        for value in (True, 0, -1, 61, "30"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                Manager({}, "pipeline", allow_paid_pipeline=True, session_seconds=value)
    def test_paid_pipeline_silence_disabled_without_changing_providers(self):
        def paid_api(cfg, path, method="GET", body=None):
            if path == "/join":
                self.assertEqual(body["pipeline_id"], "published")
                self.assertEqual(body["properties"]["parameters"]["silence_config"], {"timeout_ms": 0})
                self.assertEqual(body["properties"]["parameters"]["output_audio_codec"], "G722")
                self.assertEqual(body["properties"]["remote_rtc_uids"], ["1"])
                self.assertNotIn("tts", body["properties"])
                self.assertNotIn("asr", body["properties"])
                self.assertNotIn("turn_detection", body["properties"])
                return {"agent_id": "paid_mock"}
            return {}
        manager = Manager(self.manager.cfg, "published", api=paid_api,
                          mint=self.manager.mint, allow_paid_pipeline=True, session_seconds=30)
        with patch("session_server.control.free_llm_config", return_value={"params": {"model": "mock"}}):
            session = manager.start()
        manager.stop(session["lease"])
        self.assertEqual(manager.starts, 1)
        self.assertFalse(manager.sessions)
    def tts_override(self):
        return {"vendor": "bytedance_duplex", "params": {
            "studio_agent_resource_uuid": "a" * 32, "speaker": "test_voice",
            "audio_params": {"speech_rate": -15}}}
    def test_paid_tts_rate_override_preserves_working_voice_path(self):
        original = self.tts_override()
        expected = self.tts_override()
        bodies = []
        def api(cfg, path, method="GET", body=None):
            if path == "/join":
                bodies.append(body)
                self.assertEqual(body["pipeline_id"], "published")
                self.assertEqual(body["properties"]["tts"], expected)
                self.assertEqual(body["properties"]["parameters"]["output_audio_codec"], "G722")
                self.assertEqual(body["properties"]["parameters"]["silence_config"], {"timeout_ms": 0})
                self.assertNotIn("asr", body["properties"])
                self.assertNotIn("turn_detection", body["properties"])
                return {"agent_id": "rate_mock"}
            return {}
        manager = Manager(self.manager.cfg, "published", api=api,
                          mint=self.manager.mint, allow_paid_pipeline=True,
                          session_seconds=30, tts_override=original)
        original["params"]["audio_params"]["speech_rate"] = 100
        with patch("session_server.control.free_llm_config", return_value={"params": {"model": "mock"}}):
            session = manager.start()
        manager.stop(session["lease"])
        bodies[0]["properties"]["tts"]["params"]["audio_params"]["speech_rate"] = 0
        self.assertEqual(manager.tts_override, expected)
        self.assertFalse(manager.sessions)
        with self.assertRaises(RuntimeError): manager.start()
    def test_tts_override_cannot_bypass_free_gate(self):
        with self.assertRaises(ValueError):
            Manager({}, "published", tts_override=self.tts_override())
    def test_tts_override_validation_before_any_join(self):
        for value in (True, None, -51, 101, -15.5, "-15"):
            override = self.tts_override()
            override["params"]["audio_params"]["speech_rate"] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                Manager({}, "published", allow_paid_pipeline=True, tts_override=override)
        for field, value in (("vendor", "generic_http"), ("params", {})):
            override = self.tts_override(); override[field] = value
            with self.assertRaises(ValueError):
                Manager({}, "published", allow_paid_pipeline=True, tts_override=override)
        override = self.tts_override(); override["params"]["audio_params"]["sample_rate"] = 24000
        with self.assertRaises(ValueError):
            Manager({}, "published", allow_paid_pipeline=True, tts_override=override)
        self.assertFalse(self.cloud.calls)
    def test_board_codec_override_preserves_other_voice_settings(self):
        parameters = {"output_audio_codec": "OPUS", "transcript": {"enable": False},
                      "silence_config": {"timeout_ms": 1000}}
        original_voice = self.manager.voice()
        original_voice["parameters"] = parameters
        self.manager.voice = lambda: original_voice
        def api(cfg, path, method="GET", body=None):
            if path == "/join":
                self.assertEqual(body["properties"]["parameters"], {
                    "output_audio_codec": "G722", "transcript": {"enable": False},
                    "silence_config": {"timeout_ms": 0}})
                self.assertEqual(body["properties"]["tts"], original_voice["tts"])
                self.assertEqual(body["properties"]["turn_detection"], original_voice["turn_detection"])
                return {"agent_id": "codec_mock"}
            return {}
        self.manager.api = api
        session = self.manager.start()
        self.manager.stop(session["lease"])
        self.assertEqual(parameters["output_audio_codec"], "OPUS")
        self.assertEqual(parameters["silence_config"]["timeout_ms"], 1000)
        self.assertEqual(self.manager.starts, 1)
    def test_expiry_and_failed_cleanup(self):
        session = self.manager.start()
        self.manager.sessions[session["lease"]]["deadline"] = 0
        self.cloud.fail_leave = True
        with redirect_stdout(io.StringIO()) as captured:
            self.manager.reap()
        self.assertIn("Agent cleanup failed", captured.getvalue())
        self.assertTrue(self.manager.sessions)
        self.cloud.fail_leave = False
        self.manager.reap()
        self.assertFalse(self.manager.sessions)
    def test_start_failure(self):
        self.cloud.fail_join = True
        with self.assertRaises(RuntimeError): self.manager.start()
        self.assertFalse(self.manager.sessions)
        self.cloud.fail_join = False
        self.manager.mint = lambda cfg, channel, uid, ttl: (_ for _ in ()).throw(ValueError()) if uid == 1 else "token"
        with self.assertRaises(ValueError): self.manager.start()
        self.assertFalse(self.manager.sessions)
        self.assertTrue(self.cloud.calls[-1].endswith("/leave"))
    def test_late_keepalive_does_not_revive_expired_session(self):
        session = self.manager.start()
        self.manager.sessions[session["lease"]]["deadline"] = 0
        with self.assertRaises(ValueError): self.manager.keepalive(session["lease"])
        self.manager.reap()
        self.assertFalse(self.manager.sessions)
    def test_failed_explicit_stop_is_retried(self):
        session = self.manager.start()
        self.cloud.fail_leave = True
        with self.assertRaises(RuntimeError): self.manager.stop(session["lease"])
        with self.assertRaises(ValueError): self.manager.keepalive(session["lease"])
        self.assertEqual(self.manager.sessions[session["lease"]]["deadline"], 0)
        self.cloud.fail_leave = False
        self.manager.reap()
        self.assertFalse(self.manager.sessions)
    def test_token_lifetime_cannot_be_extended(self):
        session = self.manager.start()
        self.manager.sessions[session["lease"]]["end"] = 0
        with self.assertRaises(ValueError): self.manager.keepalive(session["lease"])
        self.manager.reap()
        self.assertFalse(self.manager.sessions)
    def test_already_deleted_agent_is_clean(self):
        session = self.manager.start()
        self.manager.api = lambda *args: (_ for _ in ()).throw(RuntimeError("Agora HTTP 404"))
        self.assertTrue(self.manager.stop(session["lease"])["ok"])
        self.assertFalse(self.manager.sessions)
    def test_expiry_and_device_stop_share_one_leave(self):
        session = self.manager.start()
        self.manager.sessions[session["lease"]]["end"] = 0
        entered = threading.Event()
        release = threading.Event()
        duplicate = threading.Event()
        device_started = threading.Event()
        calls, failures, replies = [], [], []
        def api(cfg, path, method="GET", body=None):
            calls.append(path)
            if len(calls) > 1:
                duplicate.set()
                raise RuntimeError("duplicate cloud leave")
            entered.set()
            if not release.wait(5):
                raise RuntimeError("test leave timed out")
            return {}
        self.manager.api = api
        def stop_device():
            device_started.set()
            try:
                replies.append(self.manager.stop(session["lease"]))
            except Exception as error:
                failures.append(error)
        reaper = threading.Thread(target=self.manager.reap)
        device = threading.Thread(target=stop_device)
        reaper.start()
        try:
            self.assertTrue(entered.wait(5))
            device.start()
            self.assertTrue(device_started.wait(5))
            # Hold the cloud response while the device races the expiry stop.
            duplicate.wait(0.3)
        finally:
            release.set()
            reaper.join(5)
            if device.ident is not None: device.join(5)
        self.assertFalse(reaper.is_alive() or device.is_alive())
        self.assertEqual(len(calls), 1)
        self.assertFalse(failures)
        self.assertEqual(replies, [{"ok": True}])
        self.assertFalse(self.manager.sessions)
    def test_device_config_preserves_volume_and_replaces_atomically(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "agora-lan.cfg"
            save_device_config(path, self.manager.cfg, "192.168.1.2", 8766, "b" * 64)
            self.assertEqual(json.loads(path.read_text())["volume"], 2)
            previous = json.loads(path.read_text())
            previous["volume"] = 17
            path.write_text(json.dumps(previous))
            save_device_config(path, self.manager.cfg, "192.168.1.3", 8767, "b" * 64)
            saved = json.loads(path.read_text())
            self.assertEqual((saved["volume"], saved["server_host"], saved["server_port"]), (17, "192.168.1.3", 8767))
            self.assertFalse(path.with_suffix(".cfg.tmp").exists())
            for volume in (True, -1, 101, "55"):
                saved["volume"] = volume
                path.write_text(json.dumps(saved))
                original = path.read_bytes()
                with self.assertRaises(ValueError):
                    save_device_config(path, self.manager.cfg, "192.168.1.4", 8768, "b" * 64)
                self.assertEqual(path.read_bytes(), original)
    def test_failed_bind_does_not_change_device_config(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / "config"
            config.mkdir()
            (config / "agora-pipeline.json").write_text(json.dumps({"pipeline_id": "test_pipeline"}))
            (config / "agora-service-key.txt").write_text("b" * 64)
            device = config / "agora-lan.cfg"
            device.write_text(json.dumps({"volume": 17, "server_host": "192.168.1.2"}))
            before = device.read_bytes()
            with patch("session_server.control.ROOT", root), \
                 patch("session_server.control.account", return_value=self.manager.cfg), \
                 patch("sys.argv", ["session_server.py", "--bind", "192.168.1.3"]), \
                 patch("session_server.LANServer", side_effect=OSError("test port busy")), \
                 self.assertRaises(OSError):
                main()
            self.assertEqual(device.read_bytes(), before)
            self.assertEqual((config / "agora-service-key.txt").read_text(), "b" * 64)
    def test_http(self):
        server = LANServer(("127.0.0.1", 0), handler(self.manager, "b" * 64))
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        base = "http://127.0.0.1:" + str(server.server_port)
        def post(path, body, auth="b" * 64):
            req = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                                         headers={"Authorization": "Bearer " + auth})
            with urllib.request.urlopen(req, timeout=2) as response:
                return json.load(response)
        try:
            with self.assertRaises(urllib.error.HTTPError) as error:
                post("/session/start", {}, "wrong")
            self.assertEqual(error.exception.code, 401)
            for raw, path, extra, expected in (
                    (b"{", "/session/start", {}, 400),
                    (b"[]", "/session/start", {}, 400),
                    (b"{}", "/missing", {}, 404),
                    (b"{}", "/session/start", {"Content-Length": "1025"}, 413),
                    (b"{}", "/session/start", {"Content-Length": "bad"}, 400),
                    (b"{}", "/session/start", {"Transfer-Encoding": "chunked"}, 413)):
                headers = {"Authorization": "Bearer " + "b" * 64, **extra}
                with self.subTest(path=path, expected=expected):
                    if extra:
                        # These headers must be rejected before body reads.
                        # Do not send an incomplete/mismatched body: closing
                        # with unread bytes can cause a Windows TCP reset.
                        connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=2)
                        try:
                            connection.putrequest("POST", path)
                            for name, value in headers.items(): connection.putheader(name, value)
                            connection.endheaders()
                            response = connection.getresponse()
                            self.assertEqual(response.status, expected)
                            response.read()
                        finally: connection.close()
                    else:
                        request = urllib.request.Request(base + path, data=raw, headers=headers)
                        with self.assertRaises(urllib.error.HTTPError) as error:
                            urllib.request.urlopen(request, timeout=2)
                        self.assertEqual(error.exception.code, expected)
            self.assertFalse(self.cloud.calls)
            session = post("/session/start", {})
            self.assertTrue(post("/session/keepalive", {"lease": session["lease"]})["ok"])
            with self.assertRaises(urllib.error.HTTPError) as error:
                post("/session/stop", {"lease": "bad"})
            self.assertEqual(error.exception.code, 400)
            self.assertTrue(post("/session/stop", {"lease": session["lease"]})["ok"])
        finally:
            server.shutdown()
            worker.join()
            server.server_close()


class SceneHandoffTests(unittest.TestCase):
    def setUp(self):
        self.broker = SceneHandoff(timeout=0.4)
        self.lease, self.token = "a" * 64, "c" * 64
        self.broker.activate(self.lease, self.token, time.monotonic() + 30)
        self.threads = []

    def tearDown(self):
        self.broker.close(self.lease)
        for thread in self.threads:
            thread.join(2)
            self.assertFalse(thread.is_alive())

    def begin(self, call="call_one"):
        result = {}
        def run():
            try: result["value"] = self.broker.request(self.token, call)
            except Exception as error: result["error"] = error
        thread = threading.Thread(target=run)
        self.threads.append(thread)
        thread.start()
        with self.broker.condition:
            self.assertTrue(self.broker.condition.wait_for(lambda: self.broker.job is not None, 1))
        return thread, result

    def test_fresh_request_roundtrip_and_duplicate(self):
        thread, result = self.begin()
        command = self.broker.poll(self.lease)
        self.assertEqual(command["action"], "capture_scene")
        self.assertRegex(command["request_id"], r"^[a-f0-9]{32}$")
        self.assertIsNone(self.broker.poll("wrong"))
        self.broker.complete(command["request_id"], {"text": "画面中部可见台阶。", "navigation": False})
        thread.join(1)
        self.assertFalse(result["value"]["navigation"])
        self.assertEqual(self.broker.request(self.token, "call_one"), result["value"])
        with self.assertRaises(ValueError): self.broker.request(self.token, "call_one", "a different question")
        self.assertEqual(self.broker.calls, 1)
        self.assertIsNone(self.broker.poll(self.lease))

    def test_wrong_frame_and_parallel_request_do_not_satisfy_job(self):
        thread, result = self.begin()
        with self.assertRaises(BlockingIOError): self.broker.request(self.token, "different")
        with self.assertRaises(TimeoutError):
            self.broker.complete("b" * 32, {"text": "旧图片", "navigation": False})
        for value in ({"text": "通行安全", "navigation": True}, {"text": "", "navigation": False},
                      {"text": "字" * 81, "navigation": False}):
            with self.assertRaises(ValueError): self.broker.complete(self.broker.job["id"], value)
        self.broker.close(self.lease)
        thread.join(1)
        self.assertIsInstance(result["error"], PermissionError)

    def test_timeout_never_reuses_frame_or_reacquires_duplicate(self):
        self.broker.timeout = 0.03
        thread, result = self.begin()
        request_id = self.broker.job["id"]
        thread.join(1)
        self.assertIsInstance(result["error"], TimeoutError)
        self.assertIsNone(self.broker.poll(self.lease))
        with self.assertRaises(TimeoutError):
            self.broker.complete(request_id, {"text": "太晚的结果", "navigation": False})
        with self.assertRaises(TimeoutError): self.broker.request(self.token, "call_one")
        self.assertEqual(self.broker.calls, 1)

    def test_session_isolation_authentication_and_expiry(self):
        for token in ("wrong", "b" * 64, "中文", None):
            with self.assertRaises(PermissionError): self.broker.request(token, "call_one")
        for call in (None, "bad id", "a" * 129):
            with self.assertRaises(ValueError): self.broker.request(self.token, call)
        thread, result = self.begin()
        self.broker.close(self.lease)
        self.broker.activate("b" * 64, "d" * 64, time.monotonic() - 1)
        thread.join(1)
        self.assertIsInstance(result["error"], PermissionError)
        self.assertFalse(self.broker.accepts("a" * 32))
        with self.assertRaises(PermissionError): self.broker.request("d" * 64, "call_two")
        self.broker.close("b" * 64)

    def test_limit_and_old_duplicate(self):
        for index in range(3):
            old = self.broker.job
            self.broker.job = None
            thread, result = self.begin("call" + str(index))
            self.broker.complete(self.broker.job["id"], {"text": "桌子", "navigation": False})
            thread.join(1)
            self.assertIn("value", result)
            if old:
                with self.assertRaises(TimeoutError): self.broker.request(self.token, old["call_id"])
        with self.assertRaises(BlockingIOError): self.broker.request(self.token, "call_four")

    def test_frame_claim_prevents_duplicate_from_finishing_or_failing_owner(self):
        thread, result = self.begin()
        request_id = self.broker.job["id"]
        owner, question = self.broker.claim_frame(request_id)
        self.assertEqual(question, "")
        self.assertIsNone(self.broker.poll(self.lease))
        with self.assertRaises(BlockingIOError): self.broker.claim_frame(request_id)
        self.assertFalse(self.broker.fail_frame(request_id, object()))
        self.assertFalse(self.broker.fail_frame(request_id, None))
        self.assertEqual(self.broker.frame_deadline(request_id, owner), self.broker.job["deadline"])
        with self.assertRaises(TimeoutError): self.broker.frame_deadline(request_id, object())
        with self.assertRaises(TimeoutError):
            self.broker.complete(request_id, {"text": "重复上传", "navigation": False})
        self.broker.complete(request_id, {"text": "新画面", "navigation": False}, owner)
        self.assertFalse(self.broker.fail_frame(request_id, owner))
        thread.join(1)
        self.assertTrue(result["value"]["ok"])

    def test_failed_frame_wakes_waiter_and_duplicate_gets_same_failure(self):
        thread, result = self.begin()
        request_id = self.broker.job["id"]
        owner, _ = self.broker.claim_frame(request_id)
        self.assertTrue(self.broker.fail_frame(request_id, owner))
        thread.join(1)
        self.assertFalse(result["value"]["ok"])
        self.assertFalse(result["value"]["navigation"])
        self.assertEqual(self.broker.request(self.token, "call_one"), result["value"])
        self.assertEqual(self.broker.calls, 1)

    def test_late_upload_cannot_complete_or_fail_a_new_session(self):
        thread, result = self.begin()
        request_id = self.broker.job["id"]
        owner, _ = self.broker.claim_frame(request_id)
        self.broker.close(self.lease)
        thread.join(1)
        self.assertIsInstance(result["error"], PermissionError)
        self.broker.activate(self.lease, self.token, time.monotonic() + 30)
        self.assertFalse(self.broker.fail_frame(request_id, owner))
        with self.assertRaises(TimeoutError):
            self.broker.complete(request_id, {"text": "旧会话结果", "navigation": False}, owner)


class VisionToolManagerTests(unittest.TestCase):
    def manager(self, **kwargs):
        self.cloud = Mock(return_value={"agent_id": "test_agent"})
        self.mint = Mock(return_value="test_token")
        return Manager({"AGORA_APP_ID": "a" * 32}, "test_pipeline", api=self.cloud, mint=self.mint,
                       allow_paid_pipeline=True, scene_tool_url="https://example.com/vision/tool", **kwargs)

    def start(self, manager):
        with patch("session_server.control.free_llm_config", return_value={"system_messages": []}):
            return manager.start(vision_capable=True)

    def test_daily_standby_repeated_start_stop_and_long_conversation(self):
        manager = self.manager(product_mode=True)
        self.cloud.assert_not_called()  # Starting the controller is standby.
        self.mint.assert_not_called()
        with patch('session_server.time.monotonic', return_value=1000) as clock:
            for _ in range(5):
                session = self.start(manager)
                # Go beyond rehearsal durations with regular board heartbeats.
                for _ in range(10):
                    clock.return_value += 60
                    self.assertTrue(manager.keepalive(session['lease'])['ok'])
                    manager.reap()
                    self.assertIn(session['lease'], manager.sessions)
                manager.stop(session['lease'])
                self.assertFalse(manager.sessions)
                self.assertIsNone(manager.scene_handoff.active)
        self.assertEqual(sum(c.args[1] == '/join' for c in self.cloud.call_args_list), 5)
        self.assertEqual(sum(c.args[1].endswith('/leave') for c in self.cloud.call_args_list), 5)
        self.assertTrue(all(c.args[3] == 86400 for c in self.mint.call_args_list))

    def test_daily_disappearance_and_failed_leave_do_not_revive(self):
        manager = self.manager(product_mode=True)
        with patch('session_server.time.monotonic', return_value=1000) as clock:
            session = self.start(manager)
            clock.return_value += 91
            self.cloud.side_effect = RuntimeError('HTTP 502')
            manager.reap()
            self.assertIn(session['lease'], manager.sessions)
            self.assertIsNone(manager.scene_handoff.active)
            with self.assertRaises(ValueError): manager.keepalive(session['lease'])
            with self.assertRaises(RuntimeError): self.start(manager)
            self.cloud.side_effect = None
            manager.reap()
            self.assertFalse(manager.sessions)

    def test_daily_platform_expiry_still_stops_agent(self):
        manager = self.manager(product_mode=True)
        with patch('session_server.time.monotonic', return_value=1000) as clock:
            session = self.start(manager)
            clock.return_value = 1000 + 86340
            # Even a fresh device heartbeat cannot extend the platform limit.
            manager.sessions[session['lease']]['deadline'] = clock.return_value + 90
            with self.assertRaises(ValueError): manager.keepalive(session['lease'])
            manager.reap()
            self.assertFalse(manager.sessions)

    def test_daily_camera_more_than_three_fresh_requests_and_replay_rejection(self):
        manager = self.manager(product_mode=True)
        session = self.start(manager)
        broker = manager.scene_handoff
        token = broker.active[1]
        ids = []
        try:
            for index in range(5):
                call = 'daily_' + str(index)
                result = []
                worker = threading.Thread(target=lambda: result.append(broker.request(token, call, '医院挂号窗口')))
                worker.start()
                try:
                    with broker.condition:
                        self.assertTrue(broker.condition.wait_for(lambda: broker.job and broker.job['call_id'] == call, 1))
                    request = broker.poll(session['lease'])
                    ids.append(request['request_id'])
                    owner, question = broker.claim_frame(request['request_id'])
                    self.assertEqual(question, '医院挂号窗口')
                    broker.complete(request['request_id'], {'text': '未看到挂号窗口标识。', 'navigation': False}, owner)
                    worker.join(1)
                    self.assertTrue(result[0]['ok'])
                finally:
                    if worker.is_alive(): broker.close(session['lease'])
                    worker.join(1)
            self.assertEqual(len(set(ids)), 5)
            with self.assertRaises(TimeoutError): broker.request(token, 'daily_0', '医院挂号窗口')
        finally:
            manager.stop(session['lease'])

    def test_daily_cli_conflicts_fail_before_account_or_network(self):
        valid = ['--product-mode', '--vision', '--allow-paid-pipeline',
                 '--vision-tool-url', 'https://example.com/vision/tool']
        for flags in (['--product-mode'], valid + ['--duration', '600'],
                      valid + ['--session-duration', '60'], valid + ['--demo-sessions', '3']):
            with patch('sys.argv', ['session_server.py', *flags]), \
                 patch('session_server.control.account') as account, \
                 redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                main()
            account.assert_not_called()

    def test_daily_main_remains_ready_after_many_stopped_sessions(self):
        manager = self.manager(product_mode=True)
        manager.starts = 5
        listeners = []
        handlers = {}
        iterations = []
        def listener(*args):
            item = Mock(); listeners.append(item); return item
        def reap(all_sessions=False):
            if not all_sessions:
                iterations.append(1)
                if len(iterations) == 2: handlers[__import__('signal').SIGTERM]()
        manager.reap = Mock(side_effect=reap)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root / 'config').mkdir()
            (root / 'config/agora-pipeline.json').write_text('{"pipeline_id":"offline_test"}')
            with patch('session_server.control.ROOT', root), \
                 patch('session_server.control.account', return_value={'AGORA_APP_ID': 'a' * 32}), \
                 patch('session_server.Manager', return_value=manager), \
                 patch('session_server.LANServer', side_effect=listener), \
                 patch('session_server.select.select', return_value=([], [], [])), \
                 patch('session_server.signal.signal', side_effect=lambda sig, fn: handlers.update({sig: fn})), \
                 patch('sys.argv', ['session_server.py', '--bind', '192.168.1.3', '--product-mode',
                    '--vision', '--allow-paid-pipeline', '--vision-tool-url', 'https://example.com/vision/tool']), \
                 redirect_stdout(io.StringIO()):
                main()
        self.assertEqual(len(iterations), 2)
        self.cloud.assert_not_called(); self.mint.assert_not_called()
        for item in listeners: item.server_close.assert_called_once()
        manager.reap.assert_called_with(all_sessions=True)

    def test_capability_gate_and_optin_tool_schema(self):
        manager = self.manager()
        with self.assertRaises(ValueError): manager.start()
        self.cloud.assert_not_called()
        self.mint.assert_not_called()
        self.assertFalse(manager.sessions)
        session = self.start(manager)
        self.assertTrue(session["vision_tools"])
        self.assertNotIn("vision_long_poll", session)  # Legacyv1 remains an immediate poll.
        properties = self.cloud.call_args.args[3]["properties"]
        self.assertTrue(properties["advanced_features"]["enable_tools"])
        messages = properties["llm"]["system_messages"]
        self.assertEqual(properties['llm']['params']['temperature'], 0.2)
        self.assertEqual(len([m for m in messages if m["role"] == "system"]), 1)
        self.assertIn("摄像头已连接", messages[0]["content"])
        self.assertIn("observeScene", messages[0]["content"])
        tool = properties["llm"]["tools"][0]
        self.assertEqual(tool["function"]["name"], "observeScene")
        self.assertEqual(tool["server"]["body"], {"call_id": "{{tool_call_id}}", "question": "{{args.question}}"})
        self.assertEqual(tool["function"]["parameters"]["required"], ["question"])
        self.assertEqual(tool["execution"], {"mode": "sync"})
        self.assertNotIn("test_token", tool["server"]["headers"]["Authorization"])
        manager.stop(session["lease"])
        self.assertIsNone(manager.scene_handoff.active)

    def test_invalid_mode_and_url_rejected(self):
        for url in ("http://example.com/vision/tool", "https://user:pass@example.com/vision/tool",
                    "https://example.com/vision/tool?key=x", "https://example.com/other", "file:///vision/tool"):
            with self.subTest(url=url), self.assertRaises(ValueError):
                Manager({}, "test", allow_paid_pipeline=True, scene_tool_url=url)
        with self.assertRaises(ValueError): Manager({}, None, scene_tool_url="https://example.com/vision/tool")

    def test_scene_vad_override_does_not_mutate_other_modes(self):
        from turn_detection import vad_turn_detection
        expected = {"mode": "default", "config": {
            "speech_threshold": 0.5,
            "start_of_speech": {"mode": "vad", "vad_config": {
                "interrupt_duration_ms": 160, "prefix_padding_ms": 800}},
            "end_of_speech": {"mode": "vad", "vad_config": {"silence_duration_ms": 640}}}}
        manager = self.manager()
        session = self.start(manager)
        properties = self.cloud.call_args.args[3]["properties"]
        self.assertEqual(properties["turn_detection"], expected)
        self.assertEqual(properties["llm"]["failure_message"],
                         "这次语音请求没有处理成功，请稍后再说一次。")
        self.assertEqual(properties["filler_words"], {
            "enable": True,
            "trigger": {"fixed_time_config": {"response_wait_ms": 1500}},
            "content": {"mode": "static", "static_config": {
                "phrases": ["好的，我正在处理。"], "selection_rule": "round_robin"}}})
        properties["turn_detection"]["config"]["end_of_speech"]["mode"] = "semantic"
        self.assertEqual(vad_turn_detection(), expected)
        self.assertEqual(vad_turn_detection(300, 480)["config"]["end_of_speech"],
                         {"mode": "vad", "vad_config": {"silence_duration_ms": 480}})
        manager.stop(session["lease"])

    def test_tool_cli_cannot_enable_itself_in_silent_mode(self):
        for flags in ([], ["--vision"], ["--vision", "--disable-voice"]):
            with patch("sys.argv", ["session_server.py", "--vision-tool-url",
                      "https://example.com/vision/tool", *flags]), \
                 patch("session_server.control.account") as account, \
                 redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                main()
            account.assert_not_called()

    def test_keepalive_command_and_stop_wakes_tool(self):
        manager = self.manager()
        session = self.start(manager)
        broker = manager.scene_handoff
        token = broker.active[1]
        result = []
        def request():
            try: broker.request(token, "call_one")
            except PermissionError: result.append("cancelled")
        worker = threading.Thread(target=request)
        worker.start()
        try:
            with broker.condition:
                self.assertTrue(broker.condition.wait_for(lambda: broker.job is not None, 1))
            reply = manager.keepalive(session["lease"])
            self.assertEqual(reply["vision_request"]["request_id"], broker.job["id"])
            manager.stop(session["lease"])
            worker.join(1)
            self.assertEqual(result, ["cancelled"])
        finally:
            broker.close(session["lease"])
            worker.join(1)

    def test_gateway_rejects_every_non_tool_route(self):
        manager = self.manager()
        server = LANServer(("127.0.0.1", 0), handler(manager, "b" * 64, tool_only=True))
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        try:
            base = "http://127.0.0.1:" + str(server.server_port)
            for path in ("/session/start", "/session/stop", "/session/keepalive", "/vision/frame", "/vision/capture"):
                request = urllib.request.Request(base + path, data=b'{}',
                    headers={"Authorization": "Bearer " + "b" * 64})
                with self.assertRaises(urllib.error.HTTPError) as error:
                    urllib.request.urlopen(request, timeout=2)
                self.assertEqual(error.exception.code, 404)
            self.cloud.assert_not_called()
            self.mint.assert_not_called()
        finally:
            server.shutdown();worker.join();server.server_close()

    def test_gateway_port_cli_validation_before_credentials(self):
        for port in ("0", "65536", "8766"):
            with patch("sys.argv", ["session_server.py", "--vision", "--allow-paid-pipeline",
                      "--vision-tool-url", "https://example.com/vision/tool", "--vision-tool-port", port]), \
                 patch("session_server.control.account") as account, \
                 redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                main()
            account.assert_not_called()

    def test_main_dual_loopback_listeners_without_cloud_or_audio(self):
        real_server = LANServer
        listeners, statuses, errors = [], [], []
        ready = threading.Event()
        def factory(address, request_handler):
            listener = real_server(("127.0.0.1", 0), request_handler)
            listeners.append(listener)
            if len(listeners) == 2: ready.set()
            return listener
        def visit():
            try:
                if not ready.wait(2): raise RuntimeError("listeners not ready")
                for listener, path in ((listeners[1], "/session/start"), (listeners[0], "/missing"),
                                       (listeners[1], "/vision/tool")):
                    connection = http.client.HTTPConnection("127.0.0.1", listener.server_port, timeout=2)
                    try:
                        connection.request("POST", path, body=b'{}',
                                           headers={"Authorization": "Bearer " + "b" * 64})
                        response = connection.getresponse();statuses.append(response.status);response.read()
                    finally: connection.close()
            except Exception as error: errors.append(error)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory);(root / "config").mkdir()
            (root / "config/agora-pipeline.json").write_text('{"pipeline_id":"offline_test"}')
            (root / "config/agora-service-key.txt").write_text("b" * 64)
            worker = threading.Thread(target=visit);worker.start()
            try:
                with patch("session_server.control.ROOT", root), \
                     patch("session_server.control.account", return_value={"AGORA_APP_ID": "a" * 32}), \
                     patch("session_server.control.request") as cloud, \
                     patch("session_server.LANServer", side_effect=factory), \
                     patch("session_server.signal.signal"), redirect_stdout(io.StringIO()), \
                     patch("sys.argv", ["session_server.py", "--bind", "192.168.1.3", "--duration", "1",
                         "--vision", "--allow-paid-pipeline", "--vision-tool-url", "https://example.com/vision/tool"]):
                    main()
                cloud.assert_not_called()
            finally:
                worker.join(3)
                for listener in listeners: listener.server_close()
            self.assertFalse(worker.is_alive())
            self.assertFalse(errors)
            self.assertEqual(statuses, [404, 404, 400])

    def test_main_pending_tool_does_not_block_lan_poll(self):
        # Exercise main's actual dual-listener loop, not serve_forever().
        # All cloud boundaries are replaced; the pending tool uses real HTTP.
        manager = self.manager()
        session = self.start(manager)
        broker = manager.scene_handoff
        listeners, errors, results = [], [], []
        output = io.StringIO()
        ready = threading.Event()
        def factory(address, request_handler):
            listener = LANServer(("127.0.0.1", 0), request_handler)
            listeners.append(listener)
            if len(listeners) == 2: ready.set()
            return listener
        def post(listener, path, body, token):
            connection = http.client.HTTPConnection("127.0.0.1", listener.server_port, timeout=3)
            try:
                connection.request("POST", path, json.dumps(body),
                                   {"Authorization": "Bearer " + token})
                response = connection.getresponse()
                if response.status != 200: raise AssertionError(response.status)
                return json.loads(response.read())
            finally: connection.close()
        def tool():
            try:
                results.append(post(listeners[1], "/vision/tool",
                    {"call_id": "main_loop_fixture", "question": "look ahead"}, broker.active[1]))
            except Exception as error: errors.append(error)
        def visit():
            worker = None
            try:
                if not ready.wait(2): raise RuntimeError("listeners not ready")
                worker = threading.Thread(target=tool);worker.start()
                with broker.condition:
                    if not broker.condition.wait_for(lambda: broker.job is not None, 2):
                        raise RuntimeError("tool not queued")
                reply = post(listeners[0], "/session/keepalive", {"lease": session["lease"]}, "b" * 64)
                request_id = reply["vision_request"]["request_id"]
                owner, _ = broker.claim_frame(request_id)
                broker.complete(request_id, {"navigation": False, "text": "offline fixture"}, owner)
                worker.join(2)
                if worker.is_alive(): raise RuntimeError("tool did not finish")
                post(listeners[0], "/session/stop", {"lease": session["lease"]}, "b" * 64)
            except Exception as error: errors.append(error)
            finally:
                if worker: worker.join(4)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory);(root / "config").mkdir()
            (root / "config/agora-pipeline.json").write_text('{"pipeline_id":"offline_test"}')
            (root / "config/agora-service-key.txt").write_text("b" * 64)
            worker = threading.Thread(target=visit);worker.start()
            started = time.monotonic()
            try:
                with patch("session_server.control.ROOT", root), \
                     patch("session_server.control.account", return_value={"AGORA_APP_ID": "a" * 32}), \
                     patch("session_server.Manager", return_value=manager), \
                     patch("session_server.LANServer", side_effect=factory), \
                     patch("session_server.signal.signal"), redirect_stdout(output), \
                     patch("sys.argv", ["session_server.py", "--bind", "192.168.1.3", "--duration", "30",
                         "--trace-control", "--vision", "--allow-paid-pipeline", "--vision-tool-url", "https://example.com/vision/tool"]):
                    main()
            finally:
                worker.join(5)
            self.assertFalse(worker.is_alive())
            self.assertLess(time.monotonic() - started, 5)
            self.assertEqual(errors, [])
            self.assertEqual(len(results), 1)
            self.assertTrue(results[0]["ok"])
            trace = output.getvalue()
            events = [json.loads(line.removeprefix("R1_CONTROL ")) for line in trace.splitlines()
                      if line.startswith("R1_CONTROL ")]
            deliveries = [e for e in events if e.get("vision_request") is True]
            self.assertEqual(len(deliveries), 1)
            self.assertEqual(deliveries[0]["path"], "/session/keepalive")
            for secret in (session["lease"], "b" * 64, "look ahead", "offline fixture"):
                self.assertNotIn(secret, trace)

    def test_real_http_correlated_frame_and_tool_auth(self):
        manager = self.manager()
        session = self.start(manager)
        token = manager.scene_handoff.active[1]
        scene = Mock()
        scene.analyze.return_value = {"text": "中部可见台阶。", "navigation": False}
        server = LANServer(("127.0.0.1", 0), handler(manager, "b" * 64, scene))
        serving = threading.Thread(target=server.serve_forever)
        serving.start()
        base = "http://127.0.0.1:" + str(server.server_port)
        def post(path, raw, auth, **headers):
            req = urllib.request.Request(base + path, data=raw,
                headers={"Authorization": "Bearer " + auth, **headers})
            with urllib.request.urlopen(req, timeout=2) as response: return json.load(response)
        outcome = {}
        def tool():
            try: outcome["reply"] = post("/vision/tool", b'{"call_id":"tool_one","question":"look ahead"}', token)
            except Exception as error: outcome["error"] = error
        caller = threading.Thread(target=tool)
        try:
            for path, raw, auth, headers, status in (
                ("/vision/tool", b'{"call_id":"x","question":"look ahead"}', "b" * 64, {}, 401),
                ("/vision/tool", b'{}', token, {}, 400),
                ("/vision/frame", b'jpeg', "b" * 64,
                 {"Content-Type": "image/jpeg", "X-R1-Vision-Request": "bad"}, 409)):
                with self.assertRaises(urllib.error.HTTPError) as error: post(path, raw, auth, **headers)
                self.assertEqual(error.exception.code, status)
            scene.analyze.assert_not_called()
            caller.start()
            with manager.scene_handoff.condition:
                self.assertTrue(manager.scene_handoff.condition.wait_for(lambda: manager.scene_handoff.job is not None, 1))
            command = manager.keepalive(session["lease"])["vision_request"]
            self.assertEqual(command["question"], "look ahead")
            post("/vision/frame", b'jpeg', "b" * 64,
                 **{"Content-Type": "image/jpeg", "X-R1-Vision-Request": command["request_id"]})
            caller.join(1)
            self.assertNotIn("error", outcome)
            self.assertEqual(outcome["reply"]["text"], "中部可见台阶。")
            scene.analyze.assert_called_once()
            self.assertEqual(scene.analyze.call_args.kwargs["question"], "look ahead")
            self.assertEqual(scene.analyze.call_args.kwargs["deadline"], manager.scene_handoff.job["deadline"])
        finally:
            manager.stop(session["lease"])
            if caller.ident is not None: caller.join(2)
            server.shutdown()
            serving.join()
            server.server_close()


    def test_partial_frame_body_expires_at_original_request_deadline(self):
        manager = self.manager()
        manager.scene_handoff.timeout = 0.25
        session = self.start(manager)
        token = manager.scene_handoff.active[1]
        scene = Mock()
        server = LANServer(("127.0.0.1", 0), handler(manager, "b" * 64, scene))
        serving = threading.Thread(target=server.serve_forever)
        serving.start()
        outcome = {}
        def tool():
            try: outcome["reply"] = manager.scene_handoff.request(token, "partial_body", "look ahead")
            except Exception as error: outcome["error"] = error
        caller = threading.Thread(target=tool)
        connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=2)
        try:
            caller.start()
            with manager.scene_handoff.condition:
                self.assertTrue(manager.scene_handoff.condition.wait_for(lambda: manager.scene_handoff.job is not None, 1))
            job = manager.keepalive(session["lease"])["vision_request"]
            connection.putrequest("POST", "/vision/frame")
            for name, value in {"Authorization":"Bearer "+"b"*64, "Content-Type":"image/jpeg",
                                "Content-Length":"4", "X-R1-Vision-Request":job["request_id"]}.items():
                connection.putheader(name,value)
            connection.endheaders()
            connection.send(b"j")  # An open peer stalls after a partial body.
            started = time.monotonic()
            response = connection.getresponse()
            self.assertEqual(response.status, 504)
            response.read()
            self.assertLess(time.monotonic()-started, 1)
            caller.join(1)
            self.assertFalse(caller.is_alive())
            self.assertIsInstance(outcome["error"], TimeoutError)
            scene.analyze.assert_not_called()
        finally:
            connection.close(); manager.stop(session["lease"])
            caller.join(2); server.shutdown(); serving.join(); server.server_close()

    def test_local_capture_failure_wakes_real_http_tool_without_model_or_replay(self):
        manager = self.manager()
        session = self.start(manager)
        token = manager.scene_handoff.active[1]
        scene = Mock()
        server = LANServer(("127.0.0.1", 0), handler(manager, "b" * 64, scene))
        serving = threading.Thread(target=server.serve_forever)
        serving.start()
        base = "http://127.0.0.1:" + str(server.server_port)
        def post(path, body, auth, **headers):
            req = urllib.request.Request(base+path, data=body,
                headers={"Authorization": "Bearer "+auth, **headers})
            with urllib.request.urlopen(req, timeout=2) as reply: return json.load(reply)
        outcome = {}
        def tool():
            outcome.update(post("/vision/tool", b'{"call_id":"failed_local","question":"look ahead"}', token))
        caller = threading.Thread(target=tool)
        try:
            caller.start()
            with manager.scene_handoff.condition:
                self.assertTrue(manager.scene_handoff.condition.wait_for(lambda: manager.scene_handoff.job is not None, 1))
            job = manager.keepalive(session["lease"])["vision_request"]
            headers = {"Content-Type": "application/json", "X-R1-Vision-Request": job["request_id"]}
            with self.assertRaises(urllib.error.HTTPError) as rejected:
                post("/vision/frame", b'{"capture_failed":true}', "d"*64, **headers)
            self.assertEqual(rejected.exception.code, 401)
            self.assertTrue(caller.is_alive())
            started = time.monotonic()
            self.assertEqual(post("/vision/frame", b'{"capture_failed":true}', "b"*64, **headers), {"ok": True})
            caller.join(1)
            self.assertFalse(caller.is_alive())
            self.assertLess(time.monotonic()-started, 1)
            self.assertFalse(outcome["ok"])
            with self.assertRaises(urllib.error.HTTPError) as duplicate:
                post("/vision/frame", b'{"capture_failed":true}', "b"*64, **headers)
            self.assertEqual(duplicate.exception.code, 409)
            self.assertEqual(manager.scene_handoff.calls, 1)
            scene.analyze.assert_not_called()
        finally:
            manager.stop(session["lease"])
            caller.join(2); server.shutdown(); serving.join(); server.server_close()


class ControlReplyTransportTests(unittest.TestCase):
    def test_buffered_reply_is_flushed_before_handler_returns(self):
        # Real loopback socket; no provider, RTC, operator config or hardware.
        release = threading.Event()
        replied = threading.Event()
        observed = {}
        original = {"ok": True, "vision_request": {"remaining_ms": 9999}}
        manager = Mock()
        def keepalive(lease):
            self.assertEqual(lease, "a" * 64)
            time.sleep(0.08)  # Host sojourn is excluded from the client's RTT budget.
            return original
        manager.keepalive.side_effect = keepalive
        class DelayedReturn(handler(manager, "b" * 64)):
            def reply(self, status, data):
                super().reply(status, data)
                observed["nodelay"] = self.connection.getsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY)
                replied.set()
                release.wait(2)
        server = LANServer(("127.0.0.1", 0), DelayedReturn)
        worker = threading.Thread(target=server.serve_forever)
        connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=1)
        worker.start()
        try:
            connection.request("POST", "/session/keepalive", json.dumps({"lease": "a" * 64}),
                               {"Authorization": "Bearer " + "b" * 64})
            response = connection.getresponse()
            body = json.loads(response.read())
            self.assertEqual(response.status, 200)
            self.assertTrue(replied.wait(1))
            self.assertFalse(release.is_set())
            self.assertEqual(observed["nodelay"], 1)
            self.assertIs(type(body["server_elapsed_ms"]), int)
            self.assertGreaterEqual(body["server_elapsed_ms"], 70)
            self.assertNotIn("server_elapsed_ms", original)
        finally:
            release.set()
            connection.close()
            server.shutdown(); worker.join(); server.server_close()


class ControlTraceTests(unittest.TestCase):
    def test_trace_contains_only_endpoint_status_and_timing(self):
        output = io.StringIO()
        scene = Mock()
        scene.analyze.return_value = {"text": "PRIVATE_MODEL_TEXT", "navigation": False}
        server = LANServer(("127.0.0.1", 0), handler(None, "b" * 64, scene, trace_control=True))
        worker = threading.Thread(target=server.serve_forever)
        connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
        with redirect_stdout(output):
            worker.start()
            try:
                connection.request("POST", "/vision/frame", b"PRIVATE_FRAME_BODY", {
                    "Authorization": "Bearer " + "b" * 64, "Content-Type": "image/jpeg"})
                response = connection.getresponse()
                self.assertEqual(response.status, 200); response.read(); connection.close()
                connection.request("POST", "/unknown?PRIVATE_QUERY", b"{}", {
                    "Authorization": "Bearer " + "b" * 64})
                response = connection.getresponse()
                self.assertEqual(response.status, 404); response.read()
            finally:
                connection.close(); server.shutdown(); server.server_close(); worker.join(3)
        raw = output.getvalue()
        for secret in ("b" * 64, "PRIVATE_FRAME_BODY", "PRIVATE_MODEL_TEXT", "PRIVATE_QUERY", "Authorization"):
            self.assertNotIn(secret, raw)
        self.assertTrue(all(line.startswith(("R1_CONTROL ","R1_FRAME_TIMING ")) for line in raw.splitlines()))
        frames = [json.loads(line.removeprefix("R1_FRAME_TIMING ")) for line in raw.splitlines() if line.startswith("R1_FRAME_TIMING ")]
        self.assertEqual(len(frames),1)
        self.assertEqual(set(frames[0]),{"correlated","bytes","received_bytes","body_ms","first_ms","chunks","max_gap_ms","analysis_ms","total_ms","completed"})
        self.assertEqual(frames[0]["bytes"],len(b"PRIVATE_FRAME_BODY"))
        self.assertEqual(frames[0]["received_bytes"],len(b"PRIVATE_FRAME_BODY"))
        self.assertTrue(frames[0]["completed"])
        self.assertGreaterEqual(frames[0]["body_ms"],0)
        events = [json.loads(line.removeprefix("R1_CONTROL ")) for line in raw.splitlines() if line.startswith("R1_CONTROL ")]
        self.assertEqual([event["stage"] for event in events], ["received", "sent", "received", "sent"])
        self.assertEqual([event["path"] for event in events], ["/vision/frame", "/vision/frame", "/other", "/other"])
        self.assertEqual([event["status"] for event in events if "status" in event], [200, 404])


if __name__ == "__main__": unittest.main()
