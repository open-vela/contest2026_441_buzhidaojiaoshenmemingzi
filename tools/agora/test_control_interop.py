"""Real C control/Python HTTP interop. Loopback and synthetic inputs only.

The NuttX webclient transport, camera and cloud are substitutes. This tests
wire compatibility and cancellation, not Audio, DMA, RTC or public HTTPS.
Run on the Ubuntu workspace: python tools/agora/test_control_interop.py
"""
# SPDX-License-Identifier: Apache-2.0
from concurrent.futures import ThreadPoolExecutor
import errno
import http.client
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch

from scene_relay import SceneRelay, ProviderError
from session_server import LANServer, Manager, handler

PROJECT = Path(os.environ.get("R1_INTEROP_PROJECT", Path(__file__).resolve().parents[2])).resolve()
JPEG = (b"\xff\xd8\xff\xc0\x00\x0b\x08\x01\xe0\x02\x80\x01\x01\x11\x00"
        b"\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00\x01\xff\xd9")


@unittest.skipUnless(sys.platform == "linux" and shutil.which("cc"), "Ubuntu host compiler required")
class ControlInteropTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="r1-control-interop-")
        cls.addClassCleanup(cls.build.cleanup)
        root = Path(cls.build.name)
        cls.executable = root / "control-interop"
        cls.jpeg = root / "synthetic.jpg"
        cls.jpeg.write_bytes(JPEG)
        cjson = PROJECT.parent / "apps/netutils/cjson/cJSON"
        if not (cjson / "cJSON.c").is_file():
            raise RuntimeError("restore the locked Ubuntu workspace dependencies first")
        subprocess.run([
            "cc", "-D_DEFAULT_SOURCE", "-DCONFIG_BK7258_AP_CORE=1", "-DCONFIG_BK7258_AIDK_CAMERA=1",
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1", "-no-pie",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
            "-I" + str(PROJECT / "tools/agora/control-test-stubs"),
            "-I" + str(PROJECT / "tools/xiaozhi/audio-test-stubs"),
            "-I" + str(PROJECT / "app/vision_badge/include"), "-I" + str(cjson),
            str(Path(__file__).with_suffix(".c")),
            str(PROJECT / "app/vision_badge/src/agora_control.c"),
            str(cjson / "cJSON.c"), "-lm", "-o", str(cls.executable)], check=True)

    def setUp(self):
        # Inject every external boundary. No account(), token() or provider
        # transport can be reached by this fixture, even with local keys set.
        self.provider_patch = patch("session_server.control.free_llm_config", return_value={"system_messages": []})
        self.provider_patch.start()
        self.addCleanup(self.provider_patch.stop)
        self.cloud = Mock(return_value={"agent_id": "offline_fixture"})
        self.manager = Manager({"AGORA_APP_ID": "a" * 32}, "offline_fixture",
                               api=self.cloud, mint=Mock(return_value="offline_token"),
                               allow_paid_pipeline=True,
                               scene_tool_url="https://example.invalid/vision/tool")
        self.manager.scene_handoff.poll_wait_seconds = 0.05
        self.addCleanup(self.manager.reap, all_sessions=True)
        self.query = Mock(return_value="画面中部有台阶。")
        self.relay = SceneRelay(self.query)
        self.addCleanup(self.relay.close)
        self.servers = []
        for tool_only in (False, True):
            server = LANServer(("127.0.0.1", 0), handler(
                self.manager, "b" * 64, scene=self.relay, tool_only=tool_only))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            self.servers.append(server)
            self.addCleanup(self.close_server, server, thread)
        self.stderr = tempfile.TemporaryFile(mode="w+")
        self.addCleanup(self.stderr.close)
        self.child = subprocess.Popen([str(self.executable), str(self.servers[0].server_port), str(self.jpeg)],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr,
                                      text=True, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1"))
        self.addCleanup(self.close_child)
        self.assertEqual(self.read_line(), "READY")
        self.pool = ThreadPoolExecutor(max_workers=3)
        self.addCleanup(self.close_pool)

    def close_pool(self):
        # Wake tool callers before joining their worker and closing listeners.
        self.manager.reap(all_sessions=True)
        self.pool.shutdown(wait=True, cancel_futures=True)

    @staticmethod
    def close_server(server, thread):
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)

    def close_child(self):
        if self.child.poll() is None:
            self.child.stdin.close()
            try:
                self.child.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.wait(timeout=3)
        self.child.stdout.close()
        if not self.child.stdin.closed:
            self.child.stdin.close()

    def read_line(self):
        ready, _, _ = select.select([self.child.stdout], [], [], 4)
        if not ready:
            self.fail("C peer did not respond within four seconds")
        line = self.child.stdout.readline().strip()
        if not line:
            self.stderr.seek(0)
            self.fail("C peer exited: " + self.stderr.read()[-2000:])
        return line

    def command(self, command):
        self.child.stdin.write(command + "\n")
        self.child.stdin.flush()
        return self.read_line()

    def tool_post(self, call_id, question):
        conn = http.client.HTTPConnection("127.0.0.1", self.servers[1].server_port, timeout=3)
        try:
            token = self.manager.scene_handoff.active[1]
            conn.request("POST", "/vision/tool", json.dumps({"call_id": call_id, "question": question}).encode(),
                         {"Authorization": "Bearer " + token, "Content-Type": "application/json"})
            response = conn.getresponse()
            return response.status, json.loads(response.read())
        finally:
            conn.close()

    def begin_tool(self, call_id="fixture_one", question="帮我看看前面怎么走，我要去地铁站。"):
        future = self.pool.submit(self.tool_post, call_id, question)
        broker = self.manager.scene_handoff
        with broker.condition:
            ready = broker.condition.wait_for(
                lambda: broker.job is not None and broker.job["call_id"] == call_id, timeout=2)
        self.assertTrue(ready, "tool did not create a camera request")
        return future

    def frame_post(self, request_id, content_type="image/jpeg"):
        conn = http.client.HTTPConnection("127.0.0.1", self.servers[0].server_port, timeout=4)
        try:
            conn.request("POST", "/vision/frame", JPEG, {
                "Authorization": "Bearer " + "b" * 64, "Content-Type": content_type,
                "X-R1-Vision-Request": request_id})
            response = conn.getresponse()
            return response.status, json.loads(response.read())
        finally:
            conn.close()

    def stop(self):
        self.assertEqual(self.command("stop"), "STOP 0")
        self.assertEqual(self.child.wait(timeout=4), 0)
        self.assertFalse(self.manager.sessions)
        self.assertIsNone(self.manager.scene_handoff.active)
        self.assertEqual(self.cloud.call_count, 2)  # fixture join and leave only

    def test_fresh_frame_wrong_id_and_duplicate_use_real_wire_contract(self):
        future = self.begin_tool()
        self.assertEqual(self.command("poll"), "POLL 0 1")
        self.assertEqual(self.command("wrong-id"), f"WRONG_ID {-errno.EREMOTEIO}")
        self.query.assert_not_called()
        self.assertEqual(self.command("upload"), "UPLOAD 0")
        status, result = future.result(timeout=3)
        self.assertEqual(status, 200)
        self.assertFalse(result["navigation"])
        self.assertEqual(result["text"], "画面中部有台阶。")
        self.query.assert_called_once_with(JPEG, question="帮我看看前面怎么走，我要去地铁站。")
        self.assertEqual(self.command("upload"), f"UPLOAD {-errno.EREMOTEIO}")
        self.assertEqual(self.query.call_count, 1)
        self.assertEqual(self.command("poll"), "POLL 0 0")
        self.stop()

    def test_stop_cancels_waiting_tool_without_model_or_frame(self):
        future = self.begin_tool()
        self.assertEqual(self.command("poll"), "POLL 0 1")
        self.stop()
        self.assertEqual(future.result(timeout=3)[0], 401)
        self.query.assert_not_called()

    def test_idle_c_poll_wakes_for_new_tool_without_another_connection(self):
        broker = self.manager.scene_handoff
        broker.poll_wait_seconds = 1
        entered = threading.Event()
        original = broker.poll
        def waiting_poll(lease, wait_seconds=0):
            self.assertEqual(wait_seconds, 1)
            entered.set()
            return original(lease, wait_seconds)
        with patch.object(broker, "poll", side_effect=waiting_poll):
            pending = self.pool.submit(self.command, "poll")
            self.assertTrue(entered.wait(2))
            tool = self.begin_tool("during_long_poll", "请看眼前的牌子。")
            self.assertEqual(pending.result(timeout=2), "POLL 0 1")
        self.assertEqual(self.command("upload"), "UPLOAD 0")
        self.assertTrue(tool.result(timeout=2)[1]["ok"])
        self.stop()

    def test_leave_wakes_idle_long_poll_without_camera_or_model(self):
        broker = self.manager.scene_handoff
        broker.poll_wait_seconds = 1
        entered = threading.Event()
        original = broker.poll
        def waiting_poll(lease, wait_seconds=0):
            entered.set()
            return original(lease, wait_seconds)
        with patch.object(broker, "poll", side_effect=waiting_poll):
            pending = self.pool.submit(self.command, "poll")
            self.assertTrue(entered.wait(2))
            self.manager.stop(broker.active[0])
            self.assertEqual(pending.result(timeout=2), f"POLL {-errno.EREMOTEIO} 0")
        self.query.assert_not_called()
        self.assertFalse(self.manager.sessions)

    def test_two_questions_use_two_new_frames_without_restarting_conversation(self):
        active = self.manager.scene_handoff.active
        questions = ("前面的牌子写什么？", "现在前面有什么障碍？")
        self.query.side_effect = ("牌子写着地铁入口。", "画面中部有台阶。")
        first = self.begin_tool("first_question", questions[0])
        self.assertEqual(self.command("poll"), "POLL 0 1")
        self.assertEqual(self.command("upload"), "UPLOAD 0")
        self.assertEqual(first.result(timeout=3)[1]["text"], "牌子写着地铁入口。")
        self.assertEqual(self.command("fresh-frame"), "FRESH_FRAME")
        second = self.begin_tool("second_question", questions[1])
        self.assertEqual(self.command("poll"), "POLL 0 1")
        self.assertEqual(self.command("old-upload"), f"OLD_UPLOAD {-errno.EREMOTEIO}")
        self.assertEqual(self.query.call_count, 1)
        self.assertEqual(self.command("upload"), "UPLOAD 0")
        self.assertEqual(second.result(timeout=3)[1]["text"], "画面中部有台阶。")
        self.assertEqual(self.query.call_count, 2)
        calls = self.query.call_args_list
        self.assertNotEqual(calls[0].args[0], calls[1].args[0])
        self.assertEqual([call.kwargs["question"] for call in calls], list(questions))
        self.assertEqual(self.manager.scene_handoff.active, active)
        self.assertEqual(self.cloud.call_count, 1)  # Same lease, only the original join.
        self.stop()

    def test_expired_frame_is_rejected_before_model(self):
        self.manager.scene_handoff.timeout = 0.5
        future = self.begin_tool()
        self.assertEqual(self.command("poll"), "POLL 0 1")
        status, result = future.result(timeout=3)
        self.assertEqual(status, 200)
        self.assertFalse(result["ok"])
        self.assertFalse(result["navigation"])
        self.assertEqual(self.command("upload"), f"UPLOAD {-errno.EREMOTEIO}")
        self.query.assert_not_called()
        self.manager.scene_handoff.timeout = 10
        retry = self.begin_tool("after_camera_timeout", "重新看一眼。")
        self.assertEqual(self.command("poll"), "POLL 0 1")
        self.assertEqual(self.command("old-upload"), f"OLD_UPLOAD {-errno.EREMOTEIO}")
        self.assertEqual(self.command("fresh-frame"), "FRESH_FRAME")
        self.assertEqual(self.command("upload"), "UPLOAD 0")
        self.assertTrue(retry.result(timeout=2)[1]["ok"])
        self.assertEqual(self.cloud.call_count, 1)
        self.stop()

    def assert_failure_then_continue(self, error):
        active = self.manager.scene_handoff.active
        self.query.side_effect = [error, "画面中部有台阶。"]
        first = self.begin_tool("failed_question")
        self.assertEqual(self.command("poll"), "POLL 0 1")
        self.assertEqual(self.command("upload"), f"UPLOAD {-errno.EREMOTEIO}")
        status, result = first.result(timeout=2)
        self.assertEqual(status, 200)  # Tool returns a usable failure to the LLM.
        self.assertFalse(result["ok"])
        self.assertFalse(result["navigation"])
        self.assertIn("未能取得有效的新画面", result["text"])
        self.assertNotIn("private_provider_detail", json.dumps(result))
        self.assertIsNone(self.manager.scene_handoff.poll(active[0]))
        self.assertEqual(self.command("fresh-frame"), "FRESH_FRAME")
        second = self.begin_tool("retry_question", "现在再看一眼。")
        self.assertEqual(self.command("poll"), "POLL 0 1")
        self.assertEqual(self.command("old-upload"), f"OLD_UPLOAD {-errno.EREMOTEIO}")
        self.assertEqual(self.command("upload"), "UPLOAD 0")
        self.assertTrue(second.result(timeout=2)[1]["ok"])
        self.assertEqual(self.manager.scene_handoff.active, active)
        self.assertEqual(self.cloud.call_count, 1)
        self.assertEqual(self.query.call_count, 2)
        self.stop()

    def test_provider_error_returns_failure_and_same_session_can_retry(self):
        self.assert_failure_then_continue(ProviderError(503, "offline_fixture"))

    def test_provider_timeout_returns_failure_and_same_session_can_retry(self):
        self.assert_failure_then_continue(TimeoutError("private_provider_detail"))

    def test_invalid_provider_result_returns_failure_and_same_session_can_retry(self):
        self.assert_failure_then_continue(ValueError("private_provider_detail"))

    def test_duplicate_upload_during_inference_cannot_poison_original(self):
        entered, release = threading.Event(), threading.Event()
        def query(*args, **kwargs):
            entered.set()
            if not release.wait(3): raise TimeoutError("fixture worker not released")
            return "画面中部有台阶。"
        self.query.side_effect = query
        future = self.begin_tool()
        self.assertEqual(self.command("poll"), "POLL 0 1")
        request_id = self.manager.scene_handoff.job["id"]
        upload = self.pool.submit(self.frame_post, request_id)
        try:
            self.assertTrue(entered.wait(2))
            self.assertEqual(self.frame_post(request_id)[0], 409)
            self.assertFalse(future.done())
            self.assertIsNone(self.manager.scene_handoff.poll(self.manager.scene_handoff.active[0]))
        finally:
            release.set()
        self.assertEqual(upload.result(timeout=2)[0], 200)
        self.assertTrue(future.result(timeout=2)[1]["ok"])
        self.query.assert_called_once()
        self.stop()

    def test_stop_during_inference_discards_late_frame(self):
        entered, release = threading.Event(), threading.Event()
        def query(*args, **kwargs):
            entered.set()
            if not release.wait(3): raise TimeoutError("fixture worker not released")
            return "旧会话画面。"
        self.query.side_effect = query
        future = self.begin_tool()
        self.assertEqual(self.command("poll"), "POLL 0 1")
        upload = self.pool.submit(self.frame_post, self.manager.scene_handoff.job["id"])
        try:
            self.assertTrue(entered.wait(2))
            self.stop()
            self.assertEqual(future.result(timeout=2)[0], 401)
        finally:
            release.set()
        self.assertEqual(upload.result(timeout=2)[0], 504)
        self.assertIsNone(self.manager.scene_handoff.job)

    def test_bad_frame_metadata_finishes_tool_without_calling_model(self):
        future = self.begin_tool()
        self.assertEqual(self.command("poll"), "POLL 0 1")
        self.assertEqual(self.frame_post(self.manager.scene_handoff.job["id"], "text/plain")[0], 413)
        self.assertFalse(future.result(timeout=2)[1]["ok"])
        self.query.assert_not_called()
        self.stop()


if __name__ == "__main__":
    unittest.main(verbosity=2)
