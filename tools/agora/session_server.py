"""LAN session control; conversation audio goes directly through Agora RTC.

An optional, separately enabled local ASR wake backend receives bounded PCM
before the conversation starts. It never starts a cloud agent itself.

Account/provider credentials remain on the host. A renewable device lease
closes the cloud agent after the board disappears. For trusted LAN tests only.
"""
import argparse
import copy
import hmac
import ipaddress
import json
import os
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import secrets
import select
from wake_stream_socket import configure_wake_stream
import signal
import threading
import time
import uuid
from urllib.parse import urlsplit
import session_control as control
from vision_policy import SCENE_SYSTEM_MESSAGE
from turn_detection import vad_turn_detection

# AccessToken2 is valid for at most24h. Until both board and cloud token
# renewal are implemented, close before credential expiry rather than leave
# an apparently live lease with unusable RTC. This is not a rehearsal timer.
PRODUCT_TOKEN_SECONDS = 24 * 60 * 60
PRODUCT_SESSION_SECONDS = PRODUCT_TOKEN_SECONDS - 60


def log_session_event(event, agent_id):
    """Local correlation only: never log room names, leases, tokens or bodies."""
    safe_id = agent_id if isinstance(agent_id, str) and re.fullmatch(r"[A-Za-z0-9_-]{1,128}", agent_id) else "invalid"
    print("R1_SESSION " + json.dumps({"event": event, "agent_id": safe_id,
          "unix_ms": int(time.time() * 1000)}, separators=(",", ":")), flush=True)


def scene_failure():
    # A failed observation is a normal tool result for the LLM, not a picture.
    return {"ok": False, "navigation": False,
            "text": "本次未能取得有效的新画面，无法确认当前环境。可以继续对话或重新请求看一眼。",
            "limits": "不要沿用旧画面或猜测本次场景。"}


class SceneHandoff:
    """One fresh camera request per active voice lease; never use cached frames.

    The cloud gets a session-only tool token, NOT the LAN device credential.
    Boards explicitly opt in with vision_tool_v1 when starting their lease.
    """
    def __init__(self, timeout=10.0, max_calls=3):
        self.condition = threading.Condition()
        self.timeout = timeout
        self.poll_wait_seconds = 10.0
        self.active = None
        self.job = None
        self.calls = 0
        self.seen = set()
        self.max_calls = max_calls

    def activate(self, lease, token, end):
        with self.condition:
            if self.active is not None:
                raise RuntimeError("scene handoff already active")
            self.active = (lease, token, end)
            self.job = None
            self.calls = 0
            self.seen.clear()

    def close(self, lease):
        with self.condition:
            if self.active and self.active[0] == lease:
                self.active = None
                self.job = None
                self.condition.notify_all()

    def _live(self):
        return self.active is not None and time.monotonic() < self.active[2]

    def request(self, token, call_id, question=""):
        if not isinstance(call_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", call_id):
            raise ValueError("invalid tool call identifier")
        if not isinstance(question, str) or len(question.encode("utf-8")) > 240:
            raise ValueError("invalid scene question")
        with self.condition:
            if (not isinstance(token, str) or not re.fullmatch(r"[a-f0-9]{64}", token) or
                    not self._live() or not hmac.compare_digest(token, self.active[1])):
                raise PermissionError("inactive tool credential")
            now = time.monotonic()
            job = self.job
            if job and job["call_id"] == call_id:
                # A duplicate waits for the SAME acquisition; no replay capture.
                if question != job["question"]:
                    raise ValueError("same tool call cannot change the question")
                if now >= job["deadline"]:
                    raise TimeoutError("scene request expired")
            else:
                if call_id in self.seen:
                    raise TimeoutError("older tool call cannot acquire another frame")
                if job and "result" not in job and now < job["deadline"]:
                    raise BlockingIOError("one camera request already pending")
                if self.max_calls is not None and self.calls >= self.max_calls:
                    raise BlockingIOError("voice scene test limit reached")
                self.calls += 1
                self.seen.add(call_id)
                job = self.job = {"call_id": call_id, "id": uuid.uuid4().hex,
                                  "question": question,
                                  "deadline": min(now + self.timeout, self.active[2])}
                self.condition.notify_all()
            while self._live() and self.job is job and "result" not in job:
                remaining = job["deadline"] - time.monotonic()
                if remaining <= 0: break
                self.condition.wait(remaining)
            if not self._live() or self.job is not job:
                raise PermissionError("voice lease ended")
            if time.monotonic() >= job["deadline"] or "result" not in job:
                raise TimeoutError("no fresh frame within deadline")
            return copy.deepcopy(job["result"])

    def poll(self, lease, wait_seconds=0):
        with self.condition:
            deadline = time.monotonic() + min(max(wait_seconds, 0), 10.0)
            while self._live() and self.active[0] == lease:
                job = self.job
                if (job and "result" not in job and "frame_owner" not in job and
                        time.monotonic() < job["deadline"]):
                    return {"request_id": job["id"], "action": "capture_scene",
                            "question": job["question"],
                            "remaining_ms": max(1, int((job["deadline"] - time.monotonic()) * 1000))}
                remaining = min(deadline, self.active[2]) - time.monotonic()
                if remaining <= 0: break
                self.condition.wait(remaining)
            return None

    def accepts(self, request_id):
        with self.condition:
            return (self._live() and self.job is not None and
                    request_id == self.job["id"] and "result" not in self.job and
                    time.monotonic() < self.job["deadline"])

    def claim_frame(self, request_id):
        """Reserve one upload; duplicates must not fail another worker's frame."""
        with self.condition:
            if not self.accepts(request_id):
                raise TimeoutError("stale or mismatched frame")
            if "frame_owner" in self.job:
                raise BlockingIOError("frame already uploading")
            owner = object()
            self.job["frame_owner"] = owner
            return owner, self.job["question"]

    def _owns_frame(self, request_id, owner):
        return self.accepts(request_id) and self.job.get("frame_owner") is owner

    def frame_deadline(self, request_id, owner):
        """Original host monotonic deadline, only for the claimed upload."""
        with self.condition:
            if owner is None or not self._owns_frame(request_id, owner):
                raise TimeoutError("stale or mismatched frame")
            return self.job["deadline"]

    def fail_frame(self, request_id, owner):
        """Wake the current tool immediately; never expose provider diagnostics."""
        with self.condition:
            if owner is None or not self._owns_frame(request_id, owner):
                return False
            self.job["result"] = scene_failure()
            self.condition.notify_all()
            return True

    def complete(self, request_id, result, owner=None):
        if (not isinstance(result, dict) or result.get("navigation") is not False or
                not isinstance(result.get("text"), str) or not result["text"].strip() or
                len(result["text"].encode("utf-8")) > 240):
            raise ValueError("invalid scene observation")
        with self.condition:
            if not self._owns_frame(request_id, owner):
                raise TimeoutError("stale or mismatched frame")
            self.job["result"] = {"ok": True, "navigation": False,
                                  "text": result["text"],
                                  "limits": "单帧观察不能确定距离、行走方向或通行安全；这些是能力限制，不是画面事实或固定播报词。"}
            self.condition.notify_all()

    def question_for(self, request_id):
        with self.condition:
            if not self.accepts(request_id): raise TimeoutError("expired scene question")
            return self.job["question"]


class Manager:
    def __init__(self, cfg, pipeline_id, api=control.request, mint=control.token, llm=control.llm_config, voice=None, allow_paid_pipeline=False, session_seconds=None, tts_override=None, voice_enabled=True, scene_tool_url=None, direct_scene_speech=False, demo_sessions=None, product_mode=False):
        self.cfg, self.pipeline_id, self.api, self.mint, self.llm = cfg, pipeline_id, api, mint, llm
        if type(product_mode) is not bool or (product_mode and (
                not allow_paid_pipeline or not voice_enabled or not scene_tool_url or
                demo_sessions is not None or session_seconds is not None)):
            raise ValueError('product mode requires explicit paid voice+vision, without diagnostic limits')
        self.product_mode = product_mode
        if direct_scene_speech and (not allow_paid_pipeline or not scene_tool_url):
            raise ValueError('direct scene speech requires explicit voice-tool candidate')
        self.scene_proxy = None
        self.voice = voice or control.free_voice_config
        self.sessions = {}
        self.lock = threading.Lock()
        # Expiry reaping and the board's stop POST can reach leave together.
        # Serialize them so the second caller observes confirmed cleanup.
        self.stop_lock = threading.Lock()
        self.starts = 0
        self.allow_paid_pipeline = allow_paid_pipeline
        if type(voice_enabled) is not bool or (allow_paid_pipeline and not voice_enabled):
            raise ValueError("invalid voice mode")
        self.voice_enabled = voice_enabled
        self.scene_tool_url = scene_tool_url
        self.scene_handoff = None
        if scene_tool_url is not None:
            url = urlsplit(scene_tool_url)
            if (not allow_paid_pipeline or not voice_enabled or url.scheme != "https" or
                    not url.hostname or url.username or url.password or url.query or url.fragment or
                    url.path != "/vision/tool"):
                raise ValueError("explicit HTTPS /vision/tool URL and paid voice mode required")
            self.scene_handoff = SceneHandoff(max_calls=None if product_mode else 3)
        if direct_scene_speech:
            from scene_speech_proxy import SceneSpeechProxy
            self.scene_proxy = SceneSpeechProxy(self.scene_handoff)
        if demo_sessions is not None and (not allow_paid_pipeline or not scene_tool_url or
                type(demo_sessions) is not int or not 1 <= demo_sessions <= 3):
            raise ValueError('demo sessions require explicit paid vision mode and limit1..3')
        self.max_starts = None if product_mode else demo_sessions if demo_sessions is not None else (1 if allow_paid_pipeline else 3)
        cap = 300 if demo_sessions is not None else 60 if allow_paid_pipeline else 180
        self.session_seconds = PRODUCT_SESSION_SECONDS if product_mode else cap if session_seconds is None else session_seconds
        self.token_seconds = PRODUCT_TOKEN_SECONDS if product_mode else 3600
        if not product_mode and (type(self.session_seconds) is not int or not 1 <= self.session_seconds <= cap):
            raise ValueError("session duration exceeds test limit")
        self.tts_override = None
        if tts_override is not None:
            # Explicit local override only; never turn a free-only launch into
            # paid TTS or guess credentials from another provider. Original
            # published pipeline is untouched; removing this restores it.
            if not allow_paid_pipeline or not isinstance(tts_override, dict):
                raise ValueError("TTS override requires explicit paid pipeline")
            params = tts_override.get("params")
            if (set(tts_override) != {"vendor", "params"} or
                    tts_override.get("vendor") != "bytedance_duplex" or
                    not isinstance(params, dict) or
                    set(params) != {"studio_agent_resource_uuid", "speaker", "audio_params"}):
                raise ValueError("unsupported TTS override")
            audio = params["audio_params"]
            if (not isinstance(audio, dict) or set(audio) != {"speech_rate"} or
                    type(audio["speech_rate"]) is not int or not -50 <= audio["speech_rate"] <= 100 or
                    not isinstance(params["studio_agent_resource_uuid"], str) or
                    not re.fullmatch(r"[a-f0-9]{32}", params["studio_agent_resource_uuid"]) or
                    not isinstance(params["speaker"], str) or
                    not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", params["speaker"])):
                raise ValueError("invalid TTS resource, voice or speech rate")
            self.tts_override = copy.deepcopy(tts_override)

    def start(self, vision_capable=False, vision_poll_capable=False):
        # An image-only controller must not become audible just because a
        # free voice provider becomes available or the middle key is pressed.
        if not self.voice_enabled:
            raise ValueError("voice disabled for this controller")
        if self.scene_handoff is not None and vision_capable is not True:
            raise ValueError("board vision_tool_v1 capability required; no cloud start attempted")
        with self.lock:
            if self.sessions:
                raise RuntimeError("session already active")
            if self.max_starts is not None and self.starts >= self.max_starts:
                raise RuntimeError("session test limit reached; review quotas and billing before restarting")
            self.sessions["pending"] = {}
        channel, lease = "r1_" + uuid.uuid4().hex[:16], secrets.token_hex(32)
        agent_id = None
        tool_token = secrets.token_hex(32) if self.scene_handoff is not None else None
        try:
            # Paid original pipeline is an explicit one-minute test, never a fallback.
            if self.allow_paid_pipeline:
                if not isinstance(self.pipeline_id, str) or not self.pipeline_id:
                    raise ValueError("published pipeline missing")
                voice = {"llm": copy.deepcopy(control.free_llm_config())}
                if self.tts_override is not None:
                    voice["tts"] = copy.deepcopy(self.tts_override)
            else:
                voice = self.voice()  # Free-price/endpoint check before any join.
            body = {"name": "r1_" + uuid.uuid4().hex,
                    "properties": {"channel": channel, "agent_rtc_uid": "2",
                        "remote_rtc_uids": ["1"], "enable_string_uid": False,
                        "token": self.mint(self.cfg, channel, 2, self.token_seconds), "idle_timeout": min(30, self.session_seconds),
                        **voice}}
            # The published pipeline can inherit "silence -> think" loops.
            # Diagnostic sessions must wait for actual user speech, not issue
            # empty LLM prompts and repeated paid failure TTS while waiting.
            body["properties"]["parameters"] = {
                **voice.get("parameters", {}), "silence_config": {"timeout_ms": 0},
                # Pinned BK7258 librtsa has a G722 decoder, not an Opus one.
                # Match the original BK7258 server's parameters override;
                # the board's outgoing codec does not select cloud downlink.
                "output_audio_codec": "G722"}
            if self.allow_paid_pipeline:
                body["pipeline_id"] = self.pipeline_id
            if self.scene_handoff is not None:
                # The saved pipeline uses semantic EoS. Actual voice turns
                # produced GLM1213 (missing prompt); empty final user text
                # reproduces that error directly. Reuse our existing VAD
                # path with the documented640ms silence/800ms prefix. This
                # is a candidate, not evidence that ASR empties are fixed.
                body["properties"]["turn_detection"] = vad_turn_detection()
                body["properties"]["advanced_features"] = {"enable_tools": True}
                # Reuse Agora's documented waiting speech while the LLM or
                # synchronous camera tool is pending. It uses the existing
                # TTS/RTC playback queue; no second Audio owner or /speak
                # request that could interrupt the tool result is needed.
                body["properties"]["filler_words"] = {
                    "enable": True,
                    "trigger": {"fixed_time_config": {"response_wait_ms": 1500}},
                    "content": {"mode": "static", "static_config": {
                        "phrases": ["好的，我正在处理。"], "selection_rule": "round_robin"}}}
                body["properties"]["llm"]["tools"] = [{
                    "type": "function", "function": {"name": "observeScene",
                        "description": "使用已连接的R1实体摄像头新拍一张照片并回答本次视觉问题。用户说看看、拍照、读标牌、你能用摄像头看看吗、现在再看一眼时都使用本工具；每次重新看都须重新调用。普通聊天不调用。仅观察可见线索，不能保证通行安全或推断地图路线。",
                        "parameters": {"type": "object", "properties": {
                            "question": {"type": "string", "description": "本次观察问题，保留用户已说明的当前地点和目标；再看一眼也沿用尚未改变的目标，不超过80个汉字。"}},
                                       "required": ["question"], "additionalProperties": False}},
                    "execution": {"mode": "sync"},
                    "server": {"method": "POST", "url": self.scene_tool_url,
                        "headers": {"Authorization": "Bearer " + tool_token,
                                    "Content-Type": "application/json"},
                        "body": {"call_id": "{{tool_call_id}}", "question": "{{args.question}}"}, "timeout_ms": 12000}}]
                llm = body["properties"]["llm"]
                # Task/scene information should survive short follow-ups.
                # Keep the original provider/model/voice and reduce sampling
                # randomness only for this grounded vision dialogue profile.
                llm.setdefault('params', {})['temperature'] = 0.2
                llm["failure_message"] = "这次语音请求没有处理成功，请稍后再说一次。"
                messages = llm.setdefault("system_messages", [])
                # One grounded system message. The old generic persona plus
                # a second reminder still denied the attached camera.
                llm["system_messages"] = [{"role": "system", "content": SCENE_SYSTEM_MESSAGE}] + [
                    m for m in messages if m.get("role") != "system"]
            with self.lock:
                self.starts += 1  # A failed/ambiguous join is counted, never auto-retried.
            session_end = time.monotonic() + self.session_seconds
            if self.scene_proxy is not None:
                llm = body['properties']['llm']
                proxy_token = self.scene_proxy.activate(lease,llm,session_end)
                llm['url'],llm['api_key'] = self.scene_tool_url,proxy_token
            agent_id = self.api(self.cfg, "/join", "POST", body).get("agent_id")
            if not isinstance(agent_id, str) or not agent_id:
                raise RuntimeError("Agora returned no agent_id")
            # Preserve correlation before token minting or later cleanup can
            # fail. After leave, the active-agent list no longer contains it.
            log_session_event("joined", agent_id)
            result = {"app_id": self.cfg["AGORA_APP_ID"], "channel": channel,
                      "uid": 1, "remote_uid": 2, "volume": 2, "lease": lease,
                      "token": self.mint(self.cfg, channel, 1, self.token_seconds),
                      "expires_at": int(time.time()) + self.token_seconds}
            with self.lock:
                self.sessions.pop("pending", None)
                self.sessions[lease] = {"agent_id": agent_id, "deadline": time.monotonic() + 90,
                                        "end": session_end, "long_poll": vision_poll_capable is True}
                if self.scene_handoff is not None:
                    self.scene_handoff.activate(lease, tool_token, session_end)
                    result["vision_tools"] = True
                    if vision_poll_capable is True: result["vision_long_poll"] = True
            return result
        except Exception:
            if self.scene_proxy is not None:self.scene_proxy.close(lease)
            with self.lock:
                self.sessions.pop("pending", None)
                if agent_id:
                    self.sessions[lease] = {"agent_id": agent_id, "deadline": 0, "end": 0}
            self.reap()
            raise

    def keepalive(self, lease):
        with self.lock:
            item = self.sessions.get(lease)
            if not item or "agent_id" not in item or time.monotonic() >= min(item["deadline"], item["end"]):
                raise ValueError("unknown or expired session")
            item["deadline"] = time.monotonic() + 90
            long_poll = item.get("long_poll", False)
        result = {"ok": True}
        if self.scene_handoff is not None:
            pending = self.scene_handoff.poll(lease,
                self.scene_handoff.poll_wait_seconds if long_poll else 0)
            with self.lock:
                item = self.sessions.get(lease)
                if not item or time.monotonic() >= min(item["deadline"], item["end"]):
                    raise ValueError("voice session ended during poll")
            if pending is not None: result["vision_request"] = pending
        return result

    def stop(self, lease):
        with self.stop_lock:
            return self._stop(lease)

    def _stop(self, lease):
        with self.lock:
            item = self.sessions.get(lease)
            if item and "agent_id" in item:
                # A failed leave must be retried, never revived by a late ping.
                item["deadline"] = 0
        if not item:
            return {"ok": True}
        if self.scene_handoff is not None:
            self.scene_handoff.close(lease)
        if self.scene_proxy is not None:self.scene_proxy.close(lease)
        if "agent_id" not in item:
            raise ValueError("not ready")
        try:
            self.api(self.cfg, "/agents/" + item["agent_id"] + "/leave", "POST", {})
        except RuntimeError as error:
            if "HTTP 404" not in str(error):
                log_session_event("leave_failed", item["agent_id"])
                raise
            log_session_event("leave_absent", item["agent_id"])
        except Exception:
            log_session_event("leave_failed", item["agent_id"])
            raise
        else:
            log_session_event("leave_acknowledged", item["agent_id"])
        with self.lock:
            self.sessions.pop(lease, None)
        return {"ok": True}

    def reap(self, all_sessions=False):
        with self.lock:
            stale = [key for key, value in self.sessions.items() if key != "pending" and
                     (all_sessions or time.monotonic() >= min(value["deadline"], value["end"]))]
        for lease in stale:
            try:
                self.stop(lease)
            except Exception as error:
                # Keep failed leaves for a subsequent retry; do not lose the ID.
                print("Agent cleanup failed:", type(error).__name__, flush=True)


def handler(manager, key, scene=None, capture=None, tool_only=False, trace_control=False, wake=None, wake_udp=None):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        # CPython's StreamRequestHandler supports buffered writes plus
        # TCP_NODELAY. Flush each complete small control reply together,
        # instead of sending its headers and JSON as separate tiny writes.
        wbufsize = 4096
        disable_nagle_algorithm = True
        def log_message(self, *args):
            pass
        def trace(self, stage, status=None, vision_request=None):
            if trace_control:
                path = self.path if self.path in ("/session/start", "/session/stop", "/session/keepalive",
                    "/vision/tool", "/vision/frame", "/vision/capture") else "/other"
                event = {"stage": stage, "path": path, "unix_ms": int(time.time()*1000)}
                if status is not None:
                    event.update(status=status, elapsed_ms=int((time.monotonic()-self.started)*1000))
                if vision_request is not None:
                    event["vision_request"] = vision_request
                print("R1_CONTROL " + json.dumps(event, separators=(",", ":")), flush=True)
        def reply(self, status, data):
            if self.path == "/session/keepalive" and status == 200 and "vision_request" in data:
                data = dict(data, server_elapsed_ms=max(0, int((time.monotonic()-self.started)*1000)))
            raw = json.dumps(data, ensure_ascii=False).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(raw)
            self.wfile.flush()
            self.close_connection = True
            self.trace("sent", status, ("vision_request" in data)
                       if self.path == "/session/keepalive" and status == 200 else None)
        def do_POST(self):
            self.started = time.monotonic()
            self.trace("received")
            self.connection.settimeout(5)
            if tool_only and self.path != "/vision/tool":
                self.reply(404, {"error": "仅提供视觉工具接口"})
                return
            if self.path == "/vision/tool":
                self.scene_tool()
                return
            if not hmac.compare_digest(self.headers.get("Authorization", "").encode(), ("Bearer " + key).encode()):
                self.reply(401, {"error": "设备鉴权失败"})
                return
            if self.path.startswith("/wake/"):
                self.wake_request()
                return
            if self.path in ("/vision/frame", "/vision/capture"):
                self.scene_frame(capture if self.path.endswith("capture") else scene)
                return
            if capture is not None:
                self.reply(403, {"error": "仅照片对照模式，不允许启动语音会话"})
                return
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if not 1 <= size <= 1024 or self.headers.get("Transfer-Encoding"):
                    self.reply(413, {"error": "请求大小不正确"})
                    return
                raw = self.rfile.read(size)
                if len(raw) != size: raise ValueError("incomplete body")
                body = json.loads(raw)
                if not isinstance(body, dict): raise ValueError("not an object")
                if self.path == "/session/start":
                    if not manager.voice_enabled:
                        self.reply(403, {"error": "仅图像模式，不允许启动语音会话"})
                        return
                    result = manager.start(vision_capable=body.get("vision_tool_v1") is True,
                                           vision_poll_capable=body.get("vision_poll_v2") is True)
                elif self.path in ("/session/keepalive", "/session/stop"):
                    lease = body.get("lease")
                    if not isinstance(lease, str) or len(lease) != 64 or any(c not in "0123456789abcdef" for c in lease):
                        raise ValueError("invalid lease")
                    result = manager.stop(lease) if self.path.endswith("stop") else manager.keepalive(lease)
                else:
                    self.reply(404, {"error": "接口不存在"})
                    return
                self.reply(200, result)
            except ValueError:
                self.reply(400, {"error": "请求或会话无效"})
            except Exception as error:
                print("Session operation failed:", type(error).__name__, flush=True)
                self.reply(502, {"error": "云端会话未能完成，请检查服务日志"})
        def do_CONNECT(self):
            # NuttX webclient's existing CONNECT tunnel keeps one TCP socket.
            # This is an audio endpoint, never a forwarding proxy: authority
            # must be this listener itself; no outbound connection is made.
            self.started = time.monotonic()
            self.connection.settimeout(3)
            expected = f"{self.server.server_address[0]}:{self.server.server_address[1]}"
            if tool_only or wake is None or self.path != expected:
                self.reply(403, {"error": "wake tunnel unavailable"})
                return
            if (not hmac.compare_digest(self.headers.get("Authorization", "").encode(), ("Bearer " + key).encode()) or
                    self.headers.get("X-R1-Wake-Stream") != "1"):
                self.reply(401, {"error": "wake authorization failed"})
                return
            if self.headers.get("Transfer-Encoding") or self.headers.get("Content-Length") not in (None, "0"):
                self.reply(400, {"error": "CONNECT cannot carry an HTTP request body"})
                return
            stream = None
            try:
                immediate_ack = configure_wake_stream(self.connection)
            except (OSError, AttributeError):
                self.reply(503, {"error": "wake TCP configuration unavailable"})
                return
            if immediate_ack:
                print("R1_WAKE_TCP immediate_ack=1", flush=True)
            try:
                stream = wake.start(streaming=True)["stream"]
            except BlockingIOError:
                self.reply(429, {"error": "wake stream busy"})
                return
            except (ValueError, RuntimeError):
                self.reply(503, {"error": "wake service unavailable"})
                return
            self.close_connection = True
            try:
                self.send_response(200, "Connection Established")
                self.end_headers()
                self.wfile.flush()
                if self.rfile.read(4) != b"R1A2":
                    raise ValueError("invalid wake stream preamble")
                def send(data):
                    raw = json.dumps(data, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
                    if len(raw) > 1024: raise ValueError("oversized wake reply")
                    self.wfile.write(raw)
                    self.wfile.flush()
                send({"protocol": 2, "stream": stream})
                sequence = chunks = 0
                pcm = bytearray()
                while True:
                    # Fixed 20ms PCM frame, network-order sequence prefix.
                    # BufferedReader handles short TCP reads; no audio file.
                    raw = self.rfile.read(644)
                    if not raw: break
                    if len(raw) != 644 or int.from_bytes(raw[:4], "big") != sequence:
                        raise ValueError("stream frame sequence mismatch")
                    sequence += 1
                    pcm.extend(raw[4:])
                    if len(pcm) < 16000: continue
                    result = wake.feed(stream, chunks, bytes(pcm))
                    pcm.clear()
                    chunks += 1
                    result["last_frame"] = sequence - 1
                    send(result)
                    if result["matched"]: wake.reset_decoder(stream)
            except (OSError, TimeoutError, ValueError):
                # Disconnect, a stalled stream or malformed input discards
                # decoder state; it cannot start an RTC agent.
                pass
            finally:
                if stream is not None: wake.stop(stream)
        def wake_request(self):
            if wake is None:
                self.reply(503, {"error": "local wake disabled"})
                return
            try:
                size = int(self.headers.get("Content-Length", "0"))
                frame = self.path == "/wake/frame"
                if (self.headers.get("Transfer-Encoding") or
                        (frame and (size != 16000 or self.headers.get("Content-Type") != "application/octet-stream")) or
                        (not frame and not 1 <= size <= 128)):
                    self.reply(413, {"error": "invalid wake body"})
                    return
                raw = self.rfile.read(size)
                if len(raw) != size:
                    raise ValueError("incomplete wake body")
                if frame:
                    match = re.fullmatch(r"([a-f0-9]{32}):([0-9]{1,9})", self.headers.get("X-R1-Wake", ""))
                    if not match:
                        raise ValueError("invalid wake sequence")
                    result = wake.feed(match[1], int(match[2]), raw)
                elif self.path == "/wake/start" and json.loads(raw) == {}:
                    result = wake.start()
                elif self.path == "/wake/udp-start" and wake_udp is not None and json.loads(raw) == {}:
                    result = wake_udp.start(self.client_address[0])
                elif self.path == "/wake/stop":
                    body = json.loads(raw)
                    if (not isinstance(body, dict) or set(body) != {"stream"} or
                            not isinstance(body["stream"], str) or not re.fullmatch(r"[a-f0-9]{32}", body["stream"])):
                        raise ValueError("invalid wake stream")
                    result = (wake_udp.stop(body["stream"]) if wake_udp is not None
                              else wake.stop(body["stream"]))
                else:
                    raise ValueError("invalid wake endpoint")
                self.reply(200, result)
            except BlockingIOError:
                self.reply(429, {"error": "wake stream busy"})
            except TimeoutError:
                self.reply(409, {"error": "wake stream expired"})
            except (ValueError, TypeError):
                self.reply(400, {"error": "invalid wake request"})
            except Exception as error:
                print("Local wake failed:", type(error).__name__, flush=True)
                self.reply(503, {"error": "local ASR failed"})
        def scene_tool(self):
            proxy = getattr(manager,'scene_proxy',None)
            if proxy is not None and self.headers.get('Authorization','').startswith('Bearer r1scene-'):
                proxy.handle(self)
                return
            # Only the dedicated endpoint may be forwarded by a future HTTPS
            # gateway. It never accepts the LAN device key as tool authority.
            broker = manager.scene_handoff
            if broker is None:
                self.reply(403, {"error": "语音视觉工具未启用"})
                return
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if not 1 <= size <= 1024 or self.headers.get("Transfer-Encoding"):
                    self.reply(413, {"error": "工具请求大小不正确"})
                    return
                raw = self.rfile.read(size)
                if len(raw) != size: raise ValueError("incomplete tool body")
                body = json.loads(raw)
                if (not isinstance(body, dict) or set(body) != {"call_id", "question"} or
                        not isinstance(body["question"], str) or not body["question"].strip()):
                    raise ValueError("invalid tool body")
                auth = self.headers.get("Authorization", "")
                if not auth.startswith("Bearer "): raise PermissionError("missing tool token")
                self.reply(200, broker.request(auth[7:], body["call_id"], body["question"]))
            except PermissionError:
                self.reply(401, {"error": "工具鉴权或当前会话无效"})
            except BlockingIOError:
                self.reply(429, {"error": "正在观察环境或已到测试上限"})
            except TimeoutError:
                # A missing/late camera frame must let the same conversation
                # continue. Expired uploads are still rejected by scene_frame.
                self.reply(200, scene_failure())
            except ValueError:
                self.reply(400, {"error": "工具请求无效"})
            except Exception:
                self.reply(502, {"error": "未取得新画面", "navigation": False})
        def scene_frame(self, relay):
            from scene_relay import Busy, ProviderError, MAX_JPEG
            started = time.monotonic()
            broker = getattr(manager, "scene_handoff", None)
            request_id = self.headers.get("X-R1-Vision-Request")
            owner = None
            deadline = None
            size = 0
            body_ms = analysis_ms = -1
            first_ms = max_gap_ms = chunks = received_bytes = 0
            completed = False
            socket_timeout = self.connection.gettimeout()
            if relay is None:
                self.reply(503, {"error": "场景识别未启用"})
                return
            try:
                if request_id is not None:
                    try:
                        if broker is None: raise TimeoutError("scene tool disabled")
                        owner, question = broker.claim_frame(request_id)
                        deadline = broker.frame_deadline(request_id, owner)
                    except (TimeoutError, BlockingIOError):
                        self.reply(409, {"error": "画面请求已过时或已有上传正在处理"})
                        return
                if request_id is not None and capture is not None and relay is capture:
                    raise ValueError("capture diagnostic cannot satisfy a scene request")
                size = int(self.headers.get("Content-Length", "0"))
                if self.headers.get("Content-Type") == "application/json":
                    # A camera can fail before an upload exists. Reuse the
                    # correlated owner/failure path instead of waiting 10s.
                    if owner is None or not 1 <= size <= 64 or self.headers.get("Transfer-Encoding"):
                        raise ValueError("invalid capture failure")
                    raw = self.rfile.read(size)
                    body = json.loads(raw)
                    if len(raw) != size or body != {"capture_failed": True} or body.get("capture_failed") is not True:
                        raise ValueError("invalid capture failure")
                    if not broker.fail_frame(request_id, owner):
                        raise TimeoutError("stale capture failure")
                    self.reply(200, {"ok": True})
                    return
                if (not 4 <= size <= MAX_JPEG or self.headers.get("Transfer-Encoding") or
                        self.headers.get("Content-Type") != "image/jpeg"):
                    if owner is not None: broker.fail_frame(request_id, owner)
                    self.reply(413, {"error": "需要不超过256KiB的JPEG"})
                    return
                body_started = previous = time.monotonic()
                try:
                    if deadline is None:
                        data = self.rfile.read(size)
                        received_bytes = len(data)
                    else:
                        data = bytearray()
                        while len(data) < size:
                            remaining = deadline-time.monotonic()
                            if remaining <= 0: raise TimeoutError("frame request expired")
                            self.connection.settimeout(min(10,remaining))
                            chunk = self.rfile.read1(min(16384,size-len(data)))
                            now = time.monotonic()
                            if not chunk: break
                            if not chunks: first_ms = int((now-body_started)*1000)
                            max_gap_ms = max(max_gap_ms,int((now-previous)*1000))
                            previous = now;chunks += 1;data.extend(chunk);received_bytes += len(chunk)
                        data = bytes(data)
                finally:
                    body_ms = int((time.monotonic()-body_started)*1000)
                if len(data) != size:
                    raise ValueError("incomplete frame")
                analysis_started = time.monotonic()
                try:
                    result = (relay.analyze(data, started, question=question, deadline=deadline)
                              if request_id is not None else relay.analyze(data, started))
                finally:
                    analysis_ms = int((time.monotonic()-analysis_started)*1000)
                if request_id is not None:
                    broker.complete(request_id, result, owner)
                completed = True
                self.reply(200, result)
            except Busy:
                self.reply(429, {"error": "识别忙或测试次数已用完，请稍后重新操作"})
            except ValueError:
                self.reply(400, {"error": "图片或模型结果不完整"})
            except TimeoutError:
                self.reply(504, {"error": "结果已过时，本次不输出场景提示"})
            except ProviderError as error:
                print(f"Scene provider HTTP={error.status} code={error.code}", flush=True)
                self.reply(502, {"error": "视觉模型接口未完成，请检查电脑端状态码"})
            except Exception as error:
                print("Scene operation failed:", type(error).__name__, flush=True)
                self.reply(502, {"error": "场景识别未完成"})
            finally:
                # No-op on success, cancellation, expiry or an unrelated upload.
                if owner is not None: broker.fail_frame(request_id, owner)
                try: self.connection.settimeout(socket_timeout)
                except OSError: pass
                print("R1_FRAME_TIMING " + json.dumps({"correlated":request_id is not None,
                      "bytes":size,"received_bytes":received_bytes,"body_ms":body_ms,"first_ms":first_ms,
                      "chunks":chunks,"max_gap_ms":max_gap_ms,"analysis_ms":analysis_ms,
                      "total_ms":int((time.monotonic()-started)*1000),"completed":completed},separators=(",",":")),flush=True)
    return Handler


class LANServer(ThreadingHTTPServer):
    # A vision request must not block voice lease renewals. Bound workers too.
    daemon_threads = False
    block_on_close = True
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.slots = threading.BoundedSemaphore(4)
    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            try:
                request.sendall(b"HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            finally:
                self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except Exception:
            self.slots.release()
            raise
    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()
    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(5)  # Includes incomplete HTTP headers.
        return connection, address


def save_device_config(path, cfg, bind, port, key):
    volume = 2
    if path.exists():
        previous = json.loads(path.read_text(encoding="utf-8-sig"))
        volume = previous.get("volume", volume)
        if type(volume) is not int or not 0 <= volume <= 100:
            raise ValueError("invalid saved volume; configuration was not replaced")
    device = {"app_id": cfg["AGORA_APP_ID"], "server_host": bind,
              "server_port": port, "device_key": key, "volume": volume}
    temporary = path.with_suffix(path.suffix + ".tmp")
    try:
        with temporary.open("w", encoding="utf-8") as output:
            json.dump(device, output, separators=(",", ":"))
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists(): temporary.unlink()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="192.168.110.25")
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--duration", type=int, default=0)
    parser.add_argument("--session-duration", type=int, help="voice seconds; paid diagnostic maximum60, explicit demo maximum300")
    parser.add_argument('--demo-sessions',type=int,help='explicit rehearsal only:1..3 manually triggered sessions, <=300s each; may charge; no automatic join')
    parser.add_argument('--product-mode', action='store_true',
                        help='daily manual/wake use: no test quotas; current RTC credentials24h, session ends60s before expiry; device heartbeat still applies')
    parser.add_argument("--vision", action="store_true", help="enable bounded JPEG-to-text, no sound")
    parser.add_argument("--trace-control", action="store_true", help="log fixed endpoint/status/timing only; no headers or request/response bodies")
    parser.add_argument("--capture-dir", help="save one diagnostic pair locally; no model calls")
    parser.add_argument("--vision-model", choices=("glm-4v-flash", "glm-4.6v-flash", "mimo-v2.6-flash"),
                        help="explicit vision model; default GLM, or MiMo with its explicit config; no fallback")
    parser.add_argument("--mimo-vision-config", help="absolute local ordinary API config; explicitly select MiMo vision only")
    parser.add_argument("--allow-paid-pipeline", action="store_true",
                        help="explicit saved Agora pipeline, ONE session/60s; TTS and other inherited services may cost money")
    parser.add_argument("--disable-voice", action="store_true",
                        help="reject voice starts before token/provider/cloud calls; image requests still work")
    parser.add_argument("--local-wake-model", help="optional pinned sherpa-onnx model directory; LAN PCM stays in RAM")
    parser.add_argument('--direct-scene-speech',action='store_true',help='opt-in trusted ordinary scene text through the same authenticated LLM callback; original TTS/history')
    parser.add_argument("--wake-trace-text", action="store_true", help="explicit diagnostic: print wake ASR text locally; never save PCM")
    parser.add_argument("--vision-tool-url", help="experimental HTTPS /vision/tool endpoint; requires new board vision_tool_v1 capability")
    parser.add_argument("--vision-tool-port", type=int, default=8767,
                        help="loopback-only tool listener; forward ONLY this port through an operator-managed HTTPS gateway")
    args = parser.parse_args()
    from scene_relay import DEFAULT_MODEL, MIMO_MODEL, load_mimo_vision_config, reviewed_vision_provider
    if args.mimo_vision_config and not args.vision:
        parser.error("--mimo-vision-config requires --vision")
    vision_model = args.vision_model or (MIMO_MODEL if args.mimo_vision_config else DEFAULT_MODEL)
    vision_provider = load_mimo_vision_config(args.mimo_vision_config) if args.mimo_vision_config else None
    reviewed_vision_provider(vision_model, vision_provider)
    if args.wake_trace_text and not args.local_wake_model:
        parser.error("wake text trace requires local wake model")
    if args.disable_voice and args.allow_paid_pipeline:
        parser.error("image-only and paid voice modes cannot be combined")
    if args.vision_tool_url and (not args.vision or not args.allow_paid_pipeline or
                                args.disable_voice or args.capture_dir):
        parser.error("vision tool requires explicit voice+vision mode; no public gateway is started")
    if args.vision_tool_url and (not 1 <= args.vision_tool_port <= 65535 or
                                 args.vision_tool_port == args.port):
        parser.error("vision tool loopback port must be separate from the LAN control port")
    address = ipaddress.ip_address(args.bind)
    networks = ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")
    if address.version != 4 or not any(address in ipaddress.ip_network(net) for net in networks):
        parser.error("explicit LAN IPv4 address required, matching the R1 client")
    if not 1 <= args.port <= 65535 or args.duration < 0:
        parser.error("port must be 1..65535 and duration must be non-negative")
    if args.capture_dir and (args.vision or args.allow_paid_pipeline or not 1 <= args.duration <= 600):
        parser.error("capture-only requires duration 1..600, without vision or paid pipeline")
    if args.direct_scene_speech and not (args.allow_paid_pipeline and args.vision and args.vision_tool_url):
        parser.error('direct scene speech requires explicit paid vision-tool candidate')
    if args.demo_sessions is not None and (not args.allow_paid_pipeline or not args.vision_tool_url or
            not 1 <= args.demo_sessions <= 3 or not 1 <= args.duration <= 1800 or
            (args.session_duration is not None and not 1 <= args.session_duration <= 300)):
        parser.error('demo needs explicit paid vision mode,1..3 sessions, bounded controller1..1800s and session1..300s')
    if args.product_mode and (not args.allow_paid_pipeline or not args.vision or not args.vision_tool_url or
            args.duration != 0 or args.demo_sessions is not None or args.session_duration is not None):
        parser.error('product mode requires paid voice+vision, duration0 and no demo/session duration limits')
    cfg = control.account()
    cfg["R1_REGION"] = "cn"
    key_path = control.ROOT / "config" / "agora-service-key.txt"
    key = key_path.read_text().strip() if key_path.exists() else secrets.token_hex(32)
    if len(key) != 64 or any(c not in "0123456789abcdef" for c in key): raise ValueError("invalid device key")
    if args.allow_paid_pipeline:
        pipeline = json.loads((control.ROOT / "config" / "agora-pipeline.json").read_text(encoding="utf-8-sig"))
        manager = Manager(cfg, pipeline.get("pipeline_id"), allow_paid_pipeline=True,
                          session_seconds=args.session_duration, tts_override=pipeline.get("tts_override"),
                          scene_tool_url=args.vision_tool_url, direct_scene_speech=args.direct_scene_speech,
                          demo_sessions=args.demo_sessions, product_mode=args.product_mode)
        if manager.tts_override is not None:
            print("Explicit local TTS speech_rate=" +
                  str(manager.tts_override["params"]["audio_params"]["speech_rate"]) +
                  "; published pipeline unchanged; cloud/audio validation required", flush=True)
        if manager.product_mode:
            print('Daily mode: standby until middle key or wake; no test quotas. Current RTC credentials24h; session closes60s before expiry (no token renewal yet). Services may charge. No automatic cloud join/rejoin.', flush=True)
        else:
            print(f"WARNING: paid published pipeline selected, at most{manager.max_starts} manually triggered sessions/{manager.session_seconds}s each; not free-only", flush=True)
        if manager.scene_handoff is not None:
            print("EXPERIMENTAL voice-camera handoff: requires vision_tool_v1 board; no HTTPS gateway started; not navigation", flush=True)
    else:
        manager = Manager(cfg, None, session_seconds=args.session_duration,
                          voice_enabled=not (args.disable_voice or args.capture_dir))
    if args.vision:
        from scene_relay import SceneRelay
        # Timed diagnostic launches keep the 20-frame/10-minute gate. An
        # untimed operator-controlled service accepts manual button requests
        # for its lifetime; this never starts a background upload loop.
        scene = SceneRelay(model=vision_model, provider=vision_provider,
                           max_requests=20 if args.duration else None,
                           lifetime_seconds=(args.duration if args.demo_sessions is not None else 600) if args.duration else None)
        print(f"R1 vision model: {scene.model}; no automatic model fallback", flush=True)
    else:
        scene = None
    capture = None
    if args.capture_dir:
        from scene_relay import FrameRecorder
        capture = FrameRecorder(args.capture_dir)
    wake = None
    if args.local_wake_model:
        from local_asr_wake import LocalWake, SherpaRecognizer
        wake = LocalWake(SherpaRecognizer(args.local_wake_model), trace_text=args.wake_trace_text)
        print("R1 local streaming ASR wake ready; computer required; no RTC started", flush=True)
    # The UDP listener shares the authenticated HTTP control port. Existing
    # HTTP / CONNECT profiles keep their protocols and one LocalWake owner.
    from wake_udp import WakeUDP
    wake_udp = None
    server = LANServer((args.bind, args.port), handler(manager, key, scene, capture, trace_control=args.trace_control, wake=wake))
    tool_server = None
    try:
        if wake is not None:
            wake_udp = WakeUDP(server.server_address, key, wake, trace=args.trace_control)
            server.RequestHandlerClass = handler(manager, key, scene, capture,
                trace_control=args.trace_control, wake=wake, wake_udp=wake_udp)
        if manager.scene_handoff is not None:
            tool_server = LANServer(("127.0.0.1", args.vision_tool_port),
                                     handler(manager, key, tool_only=True, trace_control=args.trace_control))
        # Bind first: a failed second launch must not overwrite device config.
        key_path.write_text(key, encoding="ascii")
        save_device_config(control.ROOT / "config" / "agora-lan.cfg", cfg, args.bind, args.port, key)
    except Exception:
        if wake_udp is not None: wake_udp.close()
        if tool_server is not None: tool_server.server_close()
        server.server_close()
        if scene is not None: scene.close()
        raise
    server.timeout = 1
    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    deadline = time.monotonic() + args.duration if args.duration else float("inf")
    print(f"R1 Agora LAN controller: {args.bind}:{args.port}; no audio started", flush=True)
    if tool_server is not None:
        print(f"Tool-only gateway target: http://127.0.0.1:{args.vision_tool_port}/vision/tool; no public tunnel started", flush=True)
    try:
        while not stop.is_set() and time.monotonic() < deadline:
            if tool_server is None:
                server.handle_request()
            else:
                ready, _, _ = select.select([server.socket, tool_server.socket], [], [], 1)
                for listener in (server, tool_server):
                    if listener.socket in ready: listener.handle_request()
            manager.reap()
            if wake is not None: wake.reap()
            # Default paid diagnostics allow one session; the explicit demo
            # profile allows only its bounded number of manual starts. Once
            # exhausted and cleaned, close the launcher/tunnel as well. A
            # failed leave remains tracked for cleanup retry.
            with manager.lock:
                finished = (manager.allow_paid_pipeline and manager.max_starts is not None and
                            manager.starts >= manager.max_starts and not manager.sessions)
            if finished:
                break
    finally:
        if wake_udp is not None: wake_udp.close()
        if wake is not None: wake.close()
        if manager.scene_handoff is not None:
            with manager.lock:
                leases = [lease for lease in manager.sessions if lease != "pending"]
            for lease in leases: manager.scene_handoff.close(lease)
        if tool_server is not None: tool_server.server_close()
        server.server_close()
        if scene is not None: scene.close()
        manager.reap(all_sessions=True)
        if manager.sessions:
            raise RuntimeError("Cloud cleanup incomplete; stop the remaining agent in Agora console")


if __name__ == "__main__":
    main()
