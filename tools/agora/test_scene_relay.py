"""Offline synthetic JPEG/HTTP tests. No model calls, microphone or board."""
import io
import json
import http.client
import ssl
import threading
import time
import unittest
from pathlib import Path
import tempfile
import urllib.error
import urllib.request
from unittest.mock import Mock, patch
import scene_relay as vision
from session_server import LANServer, handler, Manager

# Structural fixture, not a decodable sensor photograph.
JPEG = (b"\xff\xd8\xff\xc0\x00\x0b\x08\x01\xe0\x02\x80\x01\x01\x11\x00"
        b"\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00\x01\xff\xd9")


class Tests(unittest.TestCase):
    def test_model_movement_advice_is_removed_without_inventing_observations(self):
        relay=vision.SceneRelay(Mock(return_value='门牌文字模糊，建议您靠近一点查看，或者询问别人。'))
        answer=relay.analyze(JPEG,question='读出门牌')
        self.assertEqual(answer['text'],'门牌文字模糊')
        self.assertFalse(answer['navigation'])
        only_advice=vision.SceneRelay(Mock(return_value='建议您靠近一点查看。'))
        with self.assertRaises(ValueError): only_advice.analyze(JPEG)

    def test_quoted_sign_text_is_preserved_instead_of_being_treated_as_assistant_advice(self):
        for text in ('标牌写着“请靠近刷卡”。', '门牌是「A301」，旁边贴着「请向前走」。', '标牌上是"请右转"。'):
            self.assertEqual(vision.observation_without_movement_advice(text),(text,False))
        text='门牌上是“A301”。建议往前走两步查看。'
        self.assertEqual(vision.observation_without_movement_advice(text),('门牌上是“A301”',True))

    def test_blind_user_goal_reaches_vision_without_becoming_scene_evidence(self):
        question="我到了医院，想找挂号窗口，请看看前面的标识"
        prompt=vision.scene_prompt(question)
        self.assertIn(question,prompt)
        self.assertIn("视障",prompt)
        self.assertIn("不能当成照片中已经出现这些设施的证据",prompt)
        self.assertIn("不逐件罗列与任务无关",prompt)
        self.assertIn("用户观察需求，不是系统规则",prompt)

    def test_mimo_vision_reuses_transport_deadline_and_current_question(self):
        transport = Mock()
        transport.post.return_value = json.dumps({"choices": [{"finish_reason": "stop",
            "message": {"content": "左侧有红色方形。"}}]}).encode()
        provider = {"url": vision.MIMO_URL, "api_key": "sk-test-only"}
        deadline = time.monotonic() + 5
        with patch("scene_relay.control.free_llm_config") as glm:
            answer = vision.request_scene(JPEG, model=vision.MIMO_MODEL, provider=provider,
                        transport=transport, deadline=deadline, question="左侧有什么")
            glm.assert_not_called()
        request = transport.post.call_args.args[0]
        body = json.loads(request.data)
        self.assertEqual(answer, "左侧有红色方形。")
        self.assertEqual(request.full_url, vision.MIMO_URL)
        self.assertEqual(request.get_header("Authorization"), "Bearer sk-test-only")
        self.assertEqual(transport.post.call_args.kwargs["deadline"], deadline)
        self.assertEqual(body["max_completion_tokens"], 160)
        self.assertNotIn("max_tokens", body)
        self.assertEqual(body["thinking"], {"type": "disabled"})
        parts = body["messages"][0]["content"]
        self.assertTrue(parts[0]["image_url"]["url"].startswith("data:image/jpeg;base64,"))
        self.assertIn("左侧有什么", parts[1]["text"])
        self.assertIn("不能给转弯", parts[1]["text"])

    def test_mimo_rejected_request_never_falls_back_or_leaks_provider_body(self):
        provider = {"url": vision.MIMO_URL, "api_key": "sk-test-only"}
        connection, response = self.connection_fixture(status=402)
        response.read.return_value = b'{"error":{"message":"private-provider-detail"}}'
        with patch("scene_relay.control.free_llm_config") as glm, \
             patch("scene_relay.http.client.HTTPSConnection", return_value=connection):
            relay = vision.SceneRelay(model=vision.MIMO_MODEL, provider=provider)
            with self.assertRaises(vision.ProviderError) as caught:
                relay.analyze(JPEG, deadline=time.monotonic() + 5)
            self.assertEqual(caught.exception.status, 402)
            self.assertNotIn("private-provider-detail", str(caught.exception))
            self.assertEqual(connection.request.call_count, 1)
            glm.assert_not_called()
            relay.close()

    def test_mimo_requires_explicit_ordinary_api_and_fixed_endpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "private.json"
            path.write_text(json.dumps({"url": vision.MIMO_URL, "api_key": "sk-test-only"}))
            provider = vision.load_mimo_vision_config(path)
            vision.SceneRelay(Mock(), model=vision.MIMO_MODEL, provider=provider).close()
        with patch("scene_relay.control.free_llm_config") as glm:
            for provider, model in [(None, vision.MIMO_MODEL),
                    ({"url": "https://other.invalid/v1/chat/completions", "api_key": "sk-test"}, vision.MIMO_MODEL),
                    ({"url": vision.MIMO_URL, "api_key": "tp-test"}, vision.MIMO_MODEL),
                    ({"url": vision.MIMO_URL, "api_key": "sk-test"}, vision.DEFAULT_MODEL)]:
                with self.assertRaises(ValueError): vision.SceneRelay(model=model, provider=provider)
            with self.assertRaises(ValueError): vision.load_mimo_vision_config("relative.json")
            glm.assert_not_called()

    def test_correlated_slow_upload_uses_remaining_original_deadline(self):
        query=Mock(return_value="新画面")
        relay=vision.SceneRelay(query)
        now=time.monotonic()
        result=relay.analyze(JPEG,started=now-6,question="look",deadline=now+2)
        self.assertEqual(result["text"],"新画面")
        query.assert_called_once_with(JPEG,question="look")
        manual=vision.SceneRelay(Mock(return_value="unused"))
        with self.assertRaises(TimeoutError): manual.analyze(JPEG,started=now-6)
        manual.query.assert_not_called()

    def test_expired_or_late_correlated_result_is_never_accepted(self):
        query=Mock(return_value="unused")
        relay=vision.SceneRelay(query)
        now=time.monotonic()
        with self.assertRaises(TimeoutError): relay.analyze(JPEG,deadline=now-1)
        query.assert_not_called()
        with patch("scene_relay.time.monotonic",return_value=100) as clock:
            def late(_): clock.return_value=103;return "late"
            relay=vision.SceneRelay(late)
            with self.assertRaises(TimeoutError): relay.analyze(JPEG,started=94,deadline=102)

    def test_upstream_post_deadline_rechecked_without_retry(self):
        connection,_=self.connection_fixture()
        transport=vision.SceneConnection()
        req=urllib.request.Request("https://example.invalid/chat",data=b"fixture")
        with patch("scene_relay.http.client.HTTPSConnection",return_value=connection) as factory:
            with self.assertRaises(TimeoutError):transport.post(req,deadline=time.monotonic()-1)
            factory.assert_not_called()
        with patch("scene_relay.time.monotonic",return_value=100) as clock, \
             patch("scene_relay.http.client.HTTPSConnection",return_value=connection):
            def sent(*args,**kwargs):clock.return_value=102
            connection.request.side_effect=sent
            with self.assertRaises(TimeoutError):transport.post(req,deadline=101)
        self.assertEqual(connection.request.call_count,1)
        connection.getresponse.assert_not_called()
        connection.close.assert_called_once()

    def test_voice_question_is_passed_only_to_this_fresh_image(self):
        query = Mock(return_value="画面中部有台阶。")
        relay = vision.SceneRelay(query)
        question = "帮我看看前面怎么走，我要去地铁站。"
        self.assertFalse(relay.analyze(JPEG, question=question)["navigation"])
        query.assert_called_once_with(JPEG, question=question)
        relay.analyze(JPEG)
        self.assertEqual(query.call_args.args, (JPEG,))
        self.assertFalse(query.call_args.kwargs)  # No question carried into a manual frame.
        self.assertIn("可见标识", vision.scene_prompt(question))
        self.assertTrue(vision.scene_prompt(question).startswith(vision.PROMPT))
        self.assertEqual(vision.scene_prompt(None), vision.PROMPT)
        relay.close()

    def test_bad_voice_question_rejected_before_model_and_quota(self):
        query = Mock(return_value="unused")
        relay = vision.SceneRelay(query)
        for question in ("", " ", True, "字" * 81):
            with self.assertRaises(ValueError): relay.analyze(JPEG, question=question)
        query.assert_not_called()
        self.assertEqual(relay.requests, 0)
        relay.close()

    def test_provider_receives_question_and_unchanged_safety_prompt(self):
        transport = Mock()
        transport.post.return_value = json.dumps({"choices": [{"finish_reason": "stop",
            "message": {"content": "可见蓝色标牌。"}}]}).encode()
        provider = {"url": "https://example.invalid/chat", "api_key": "test"}
        with patch("scene_relay.control.free_llm_config", return_value=provider):
            vision.request_scene(JPEG, transport=transport, question="找地铁标识")
        payload = json.loads(transport.post.call_args.args[0].data)
        text = payload["messages"][0]["content"][1]["text"]
        self.assertIn("找地铁标识", text)
        self.assertIn("不能给转弯或走几步等导航指令", text)

    def test_manual_service_has_no_hidden_ten_minute_expiry(self):
        calls = []
        relay = vision.SceneRelay(lambda _: calls.append(1) or "手动观察",
                                  max_requests=None, lifetime_seconds=None)
        for _ in range(25):
            self.assertEqual(relay.analyze(JPEG)["text"], "手动观察")
        with patch("scene_relay.time.monotonic", return_value=time.monotonic() + 3600):
            self.assertEqual(relay.analyze(JPEG)["sequence"], 26)
        self.assertEqual(len(calls), 26)
        relay.close()

    def connection_fixture(self, **response_changes):
        connection = Mock()
        response = Mock(status=200, will_close=False)
        response.read.return_value = b'{"choices": []}'
        for name, value in response_changes.items():
            setattr(response, name, value)
        connection.getresponse.return_value = response
        return connection, response

    def test_https_connection_reused_and_closed_explicitly(self):
        connection, response = self.connection_fixture()
        req = urllib.request.Request("https://example.invalid/v1/chat?test=1", data=b"test",
                                     headers={"Authorization": "Bearer test"})
        transport = vision.SceneConnection()
        with patch("scene_relay.http.client.HTTPSConnection", return_value=connection) as factory:
            self.assertEqual(transport.post(req), response.read.return_value)
            transport.post(req)
            self.assertEqual(factory.call_count, 1)
            context = factory.call_args.kwargs["context"]
            self.assertTrue(context.check_hostname)
            self.assertEqual(context.verify_mode, ssl.CERT_REQUIRED)
            self.assertEqual(connection.request.call_count, 2)
            self.assertEqual(connection.request.call_args.args[:2], ("POST", "/v1/chat?test=1"))
            connection.sock.settimeout.assert_called_with(10)
        transport.close()
        connection.close.assert_called_once()
        self.assertIsNone(transport.connection)

    def test_broken_connection_never_replays_post(self):
        connection, _ = self.connection_fixture()
        connection.getresponse.side_effect = http.client.RemoteDisconnected("test")
        transport = vision.SceneConnection()
        req = urllib.request.Request("https://example.invalid/chat", data=b"test")
        with patch("scene_relay.http.client.HTTPSConnection", return_value=connection) as factory:
            with self.assertRaises(http.client.RemoteDisconnected): transport.post(req)
            self.assertEqual(factory.call_count, 1)
            self.assertEqual(connection.request.call_count, 1)
            connection.close.assert_called_once()
            self.assertIsNone(transport.connection)
            # Only a new explicit request may reconnect.
            replacement, _ = self.connection_fixture()
            factory.return_value = replacement
            transport.post(req)
            self.assertEqual(factory.call_count, 2)
        transport.close()

    def test_idle_or_changed_origin_retires_connection_before_post(self):
        connection, _ = self.connection_fixture()
        transport = vision.SceneConnection()
        req = urllib.request.Request("https://example.invalid/chat", data=b"test")
        with patch("scene_relay.http.client.HTTPSConnection", return_value=connection) as factory, \
             patch("scene_relay.time.monotonic", return_value=100) as clock:
            transport.post(req)
            clock.return_value = 121
            transport.post(req)
            self.assertEqual(factory.call_count, 2)
            transport.post(urllib.request.Request("https://other.invalid/chat", data=b"test"))
            self.assertEqual(factory.call_count, 3)
        transport.close()

    def test_handshake_failure_sends_no_post_and_logs_only_phase_metadata(self):
        connection, _ = self.connection_fixture()
        connection.connect.side_effect = OSError("private-network-detail")
        transport = vision.SceneConnection()
        req = urllib.request.Request("https://example.invalid/private?secret=hidden", data=b'private-image',
            headers={"Authorization": "Bearer private-key"})
        output = io.StringIO()
        with patch("scene_relay.http.client.HTTPSConnection", return_value=connection), \
             patch("sys.stdout", output), self.assertRaises(OSError):
            transport.post(req)
        connection.request.assert_not_called()
        connection.close.assert_called_once()
        self.assertFalse(transport.last_timing["ok"])
        self.assertEqual(transport.last_timing["status"], 0)
        self.assertGreaterEqual(transport.last_timing["total_ms"], 0)
        for secret in ('example.invalid', 'hidden', 'private-image', 'private-key', 'private-network-detail'):
            self.assertNotIn(secret, output.getvalue())

    def test_https_redirect_error_oversize_and_close_are_not_reused(self):
        req = urllib.request.Request("https://example.invalid/chat", data=b"test")
        for status, raw, will_close, expected in (
            (302, b"redirect", False, urllib.error.HTTPError),
            (429, b'{"error":{"code":"1302"}}', False, urllib.error.HTTPError),
            (200, b"x" * 32769, False, ValueError),
            (200, b"{}", True, None),
        ):
            with self.subTest(status=status, size=len(raw)):
                connection, response = self.connection_fixture(status=status, will_close=will_close)
                response.read.return_value = raw
                transport = vision.SceneConnection()
                with patch("scene_relay.http.client.HTTPSConnection", return_value=connection):
                    if expected:
                        with self.assertRaises(expected) as caught: transport.post(req)
                        if isinstance(caught.exception, urllib.error.HTTPError): caught.exception.close()
                    else:
                        self.assertEqual(transport.post(req), raw)
                self.assertIsNone(transport.connection)
                self.assertEqual(connection.request.call_count, 1)
                connection.close.assert_called_once()

    def test_relay_owns_transport_and_redacts_provider_failure(self):
        connection, response = self.connection_fixture(status=429)
        response.read.return_value = b'{"error":{"code":"1302","message":"must-not-print"}}'
        with patch("scene_relay.control.free_llm_config", return_value={"url": "https://example.invalid/chat", "api_key": "test"}), \
             patch("scene_relay.http.client.HTTPSConnection", return_value=connection):
            relay = vision.SceneRelay()
            with self.assertRaises(vision.ProviderError) as caught: relay.analyze(JPEG)
            self.assertEqual(caught.exception.code, "1302")
            self.assertNotIn("must-not-print", str(caught.exception))
            with self.assertRaises(vision.Busy): relay.analyze(JPEG)
            self.assertEqual(connection.request.call_count, 1)
            relay.close()

    def test_transport_rejects_unverified_or_credential_urls(self):
        transport = vision.SceneConnection()
        with patch("scene_relay.http.client.HTTPSConnection") as factory:
            for url in ("http://example.invalid/chat", "https://u:p@example.invalid/chat", "https://example.invalid/chat#x"):
                with self.assertRaises(ValueError):
                    transport.post(urllib.request.Request(url, data=b"test"))
            factory.assert_not_called()

    def test_capture_only_saves_original_pair_and_never_calls_model(self):
        from PIL import Image
        raw = io.BytesIO()
        Image.new("RGB", (640, 480), "white").save(raw, "JPEG")
        data = raw.getvalue()
        with tempfile.TemporaryDirectory() as directory, \
             patch("scene_relay.request_scene", side_effect=AssertionError("must not call model")):
            recorder = vision.FrameRecorder(directory)
            for name in ("early.jpg", "later.jpg"):
                answer = recorder.analyze(data)
                self.assertEqual((recorder.directory / name).read_bytes(), data)
                self.assertEqual(answer["model_ms"], 0)
            with self.assertRaises(vision.Busy): recorder.analyze(data)
            self.assertEqual(len(list(recorder.directory.iterdir())), 2)
            second = vision.FrameRecorder(directory)
            self.assertNotEqual(recorder.directory, second.directory)
            with self.assertRaises(ValueError): second.analyze(JPEG)
            self.assertEqual(second.requests, 0)
            with self.assertRaises(ValueError): vision.FrameRecorder("relative-dir")

    def test_capture_http_is_separate_from_model_and_voice(self):
        class Recorder:
            def analyze(self, data, started): return {"text": "saved", "model_ms": 0}
        manager = Manager({}, None, api=lambda *_: self.fail("cloud must not be called"))
        server = LANServer(("127.0.0.1", 0), handler(manager, "b"*64, capture=Recorder()))
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            for path, status in (("/vision/capture", 200), ("/vision/frame", 503), ("/session/start", 403)):
                req = urllib.request.Request(f"http://127.0.0.1:{server.server_port}" + path,
                      data=JPEG, headers={"Authorization": "Bearer " + "b"*64, "Content-Type": "image/jpeg"})
                if status == 200:
                    with urllib.request.urlopen(req, timeout=2) as reply:
                        self.assertEqual(json.load(reply)["model_ms"], 0)
                else:
                    with self.assertRaises(urllib.error.HTTPError) as error:
                        urllib.request.urlopen(req, timeout=2)
                    self.assertEqual(error.exception.code, status)
        finally:
            server.shutdown(); thread.join(); server.server_close()

    def test_jpeg_boundaries(self):
        self.assertEqual(vision.jpeg_dimensions(JPEG), (640, 480))
        for bad in (b"", JPEG[:-2], b"xx" + JPEG[2:], JPEG * 10000,
                    JPEG[:9] + b"\x03\x00" + JPEG[11:],
                    JPEG.replace(b"\x00\x0b", b"\xff\xff"), JPEG[15:]):
            with self.subTest(size=len(bad)), self.assertRaises(ValueError):
                vision.jpeg_dimensions(bad)

    def test_text_and_limits(self):
        relay = vision.SceneRelay(lambda _: "楼梯。" * 200)
        answer = relay.analyze(JPEG)
        self.assertLessEqual(len(answer["text"].encode()), 240)
        self.assertFalse(answer["navigation"])
        self.assertEqual(answer["sequence"], 1)
        relay.requests = 20
        with self.assertRaises(vision.Busy): relay.analyze(JPEG)
        relay.requests = 0
        relay.deadline = 0
        with self.assertRaises(vision.Busy): relay.analyze(JPEG)

    def test_long_text_preserves_sentence_boundary(self):
        sentence = "画面中可见天花板和灯。"
        text = vision.bounded_scene_text(sentence + "无法确认其他物体" * 100)
        self.assertEqual(text, sentence)
        no_boundary = vision.bounded_scene_text("物体" * 100)
        self.assertTrue(no_boundary.endswith("…"))
        self.assertLessEqual(len(no_boundary.encode("utf-8")), vision.MAX_TEXT)
        self.assertEqual(vision.bounded_scene_text("楼梯。\n"), "楼梯。")

    def test_no_backlog_and_release_after_error(self):
        relay = vision.SceneRelay(lambda _: (_ for _ in ()).throw(RuntimeError("test")))
        relay.lock.acquire()
        with self.assertRaises(vision.Busy): relay.analyze(JPEG)
        relay.lock.release()
        with self.assertRaises(RuntimeError): relay.analyze(JPEG)
        self.assertEqual(relay.requests, 1)
        self.assertTrue(relay.lock.acquire(blocking=False))
        relay.lock.release()

    def test_stale_results_discarded(self):
        relay = vision.SceneRelay(lambda _: "结果")
        with self.assertRaises(TimeoutError): relay.analyze(JPEG, time.monotonic() - 6)
        self.assertEqual(relay.requests, 0)
        def slow(_):
            times[0] += 16
            return "旧结果"
        relay.query = slow
        times = [time.monotonic()]
        with patch("scene_relay.time.monotonic", side_effect=lambda: times[0]), self.assertRaises(TimeoutError):
            relay.analyze(JPEG)
        self.assertFalse(relay.lock.locked())

    def test_provider_body_and_incomplete_answer(self):
        answer = {"choices": [{"finish_reason": "stop", "message": {"content": "测试图案"}}]}
        class Opener:
            def open(self, req, timeout):
                body = json.loads(req.data)
                self_test.assertEqual(body["model"], vision.DEFAULT_MODEL)
                self_test.assertNotIn("thinking", body)
                self_test.assertFalse(body["stream"])
                self_test.assertIn("不能给转弯", body["messages"][0]["content"][1]["text"])
                return io.BytesIO(json.dumps(answer).encode())
        self_test = self
        with patch("scene_relay.control.free_llm_config", return_value={"url": "https://example.invalid", "api_key": "test"}), \
             patch("scene_relay.urllib.request.build_opener", return_value=Opener()):
            self.assertEqual(vision.request_scene(JPEG), "测试图案")
            answer["choices"][0]["finish_reason"] = "length"
            with self.assertRaises(ValueError): vision.request_scene(JPEG)

    def test_explicit_model_and_no_paid_model_fallback(self):
        calls = []
        class Opener:
            def open(self, req, timeout):
                body = json.loads(req.data)
                calls.append(body)
                return io.BytesIO(json.dumps({"choices": [{"finish_reason": "stop",
                    "message": {"content": "测试图案"}}]}).encode())
        with patch("scene_relay.control.free_llm_config", return_value={"url": "https://example.invalid", "api_key": "test"}), \
             patch("scene_relay.urllib.request.build_opener", return_value=Opener()):
            self.assertEqual(vision.request_scene(JPEG, model="glm-4.6v-flash"), "测试图案")
            self.assertEqual(calls[0]["thinking"]["type"], "disabled")
            relay = vision.SceneRelay(model="glm-4v-flash")
            with patch.object(relay.transport, "post", side_effect=lambda req: Opener().open(req, 10).read()):
                self.assertEqual(relay.analyze(JPEG)["text"], "测试图案")
            relay.close()
            self.assertEqual(calls[1]["model"], "glm-4v-flash")
            for paid in ("glm-4.6v-flashx", "glm-4.6v", "other"):
                with self.assertRaises(ValueError): vision.request_scene(JPEG, model=paid)
                with self.assertRaises(ValueError): vision.SceneRelay(model=paid)
            self.assertEqual(len(calls), 2)

    def test_http_auth_disable_and_validation(self):
        relay = vision.SceneRelay(lambda _: "画面中间有台阶，无法确认是否可以通行。")
        self.http_case(relay)
        self.http_case(None)

    def http_case(self, relay):
        server = LANServer(("127.0.0.1", 0), handler(None, "b" * 64, relay))
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            def post(data, auth="b" * 64, content="image/jpeg"):
                req = urllib.request.Request(f"http://127.0.0.1:{server.server_port}/vision/frame", data=data,
                      headers={"Authorization": "Bearer " + auth, "Content-Type": content})
                return urllib.request.urlopen(req, timeout=2)
            with self.assertRaises(urllib.error.HTTPError) as caught: post(JPEG, "bad")
            self.assertEqual(caught.exception.code, 401)
            if relay is None:
                with self.assertRaises(urllib.error.HTTPError) as caught: post(JPEG)
                self.assertEqual(caught.exception.code, 503)
            else:
                with post(JPEG) as result:
                    self.assertFalse(json.load(result)["navigation"])
                for data, content, code in ((b"xxxx", "image/jpeg", 400), (JPEG, "text/plain", 413)):
                    with self.assertRaises(urllib.error.HTTPError) as caught: post(data, content=content)
                    self.assertEqual(caught.exception.code, code)
        finally:
            server.shutdown()
            thread.join()
            server.server_close()

    def test_paid_route_explicit_bounded_not_automatic(self):
        calls = []
        def api(cfg, path, method, body):
            calls.append(body)
            return {"agent_id": "test"}
        manager = Manager({"AGORA_APP_ID": "a" * 32}, "saved-pipeline", api,
                          lambda *a: "token", allow_paid_pipeline=True)
        with patch("session_server.control.free_llm_config", return_value={"params": {"model": "free"}}):
            result = manager.start()
        self.assertEqual(calls[0]["pipeline_id"], "saved-pipeline")
        self.assertNotIn("tts", calls[0]["properties"])
        self.assertLessEqual(manager.sessions[result["lease"]]["end"] - time.monotonic(), 60)
        manager.stop(result["lease"])
        with self.assertRaises(RuntimeError): manager.start()

    def test_provider_error_is_bounded_and_redacted(self):
        class Opener:
            def open(self, *args, **kwargs):
                raise urllib.error.HTTPError("https://example.invalid", 429, "test", {},
                    io.BytesIO(b'{"error":{"code":"1302","message":"must-not-print"}}'))
        with patch("scene_relay.control.free_llm_config", return_value={"url": "https://example.invalid", "api_key": "test"}), \
             patch("scene_relay.urllib.request.build_opener", return_value=Opener()), \
             self.assertRaises(vision.ProviderError) as caught:
            vision.request_scene(JPEG)
        self.assertEqual(caught.exception.code, "1302")
        self.assertNotIn("must-not-print", str(caught.exception))

    def test_busy_provider_is_not_retried(self):
        calls = []
        def query(_):
            calls.append(1)
            raise vision.ProviderError(429, "1305")
        relay = vision.SceneRelay(query)
        with self.assertRaises(vision.ProviderError): relay.analyze(JPEG)
        with self.assertRaises(vision.Busy): relay.analyze(JPEG)
        self.assertEqual(len(calls), 1)

    def test_slow_scene_does_not_block_voice_control(self):
        entered, release = threading.Event(), threading.Event()
        def query(_):
            entered.set()
            if not release.wait(2): raise TimeoutError("test")
            return "测试场景"
        class Control:
            def keepalive(self, lease): return {"ok": True}
        server = LANServer(("127.0.0.1", 0), handler(Control(), "b" * 64, vision.SceneRelay(query)))
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        errors = []
        def post(path, body, content):
            req = urllib.request.Request(f"http://127.0.0.1:{server.server_port}" + path, data=body,
                  headers={"Authorization": "Bearer " + "b" * 64, "Content-Type": content})
            with urllib.request.urlopen(req, timeout=2) as response: return json.load(response)
        def scene_request():
            try: post("/vision/frame", JPEG, "image/jpeg")
            except Exception as error: errors.append(type(error).__name__)
        worker = threading.Thread(target=scene_request)
        try:
            worker.start()
            self.assertTrue(entered.wait(1))
            self.assertTrue(post("/session/keepalive", json.dumps({"lease": "c" * 64}).encode(), "application/json")["ok"])
        finally:
            release.set()
            worker.join()
            server.shutdown()
            thread.join()
            server.server_close()
        self.assertFalse(errors)


if __name__ == "__main__":
    unittest.main()
