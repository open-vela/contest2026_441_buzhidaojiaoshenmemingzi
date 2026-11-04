"""One JPEG -> short scene cues. No audio, disk images, navigation or retries."""
import base64
import http.client
import io
import json
import math
import re
import ssl
import threading
import time
import urllib.error
import urllib.request
import urllib.parse
import session_control as control

MAX_JPEG = 262144
MAX_TEXT = 240  # Fits the existing 256-byte RPMsg result, including NUL.
FREE_MODELS = ("glm-4v-flash", "glm-4.6v-flash")
DEFAULT_MODEL = "glm-4v-flash"
MIMO_MODEL = "mimo-v2.6-flash"
MIMO_URL = "https://api.xiaomimimo.com/v1/chat/completions"
PROMPT = (
    "你通过胸前摄像头，为已到达现场的视障用户说明眼前有用的信息。输出是直接给用户听的短口语，"
    "不是图片赏析、物体清单或通用AI聊天。先回答用户这次要找什么、读什么、确认什么。"
    "优先读清有关的门牌、入口指示、服务窗口/柜台和设施标识，说明它们在镜头画面中的相对位置。"
    "只陈述照片能确认的线索；用户所说的地点和目标是需求，不能当成照片中已经出现这些设施的证据。"
    "用户只说‘看看前面’而未说明目标时，优先入口、门牌、标识、楼梯等有助于理解所在位置的信息。"
    "没有这些信息时，简短说镜头主要拍到了什么；不猜医院、车站等地点，不追问后才肯观察。"
    "回答顺序是相关目标或观察结论、画面位置、能读清的文字；不确定时就说未看到或看不清。"
    "不描述门板、墙壁的一部分、无关颜色材质等零碎细节；只保留对当前任务有用的一两项。"
    "已找到相关标识就读出文字和方位后结束，不补充无关的灰色色块、空白背景或墙面。"
    "位置仅相对画面左中右，不等同用户行走方向。不清楚就说无法确认。"
    "物体类别看不清时描述外形，不猜具体名称，不补出画面中未确认的物体。"
    "不能推断画外环境、距离、是否可安全通行，不能给转弯或走几步等导航指令。"
    "文字模糊或未拍到目标时，只建议持稳镜头、调整镜头角度或询问旁人，不建议视障用户靠近、前进、后退、转身或移动位置来拍清楚。"
    "直接给结论，不用‘这张图中可以看到’‘图片显示了’‘好的’等开场，不追加客套话。"
    "一般回答一至两句、约30至50个汉字，信息少就少说。"
    "用户明确要求详细布局时，可以多补充相关方位信息；用户问一个具体物品就只重点回答该物品。"
    "读牌子时忠实复述能确认的文字，不为短句删改。普通描述不重复通用安全提醒。"
    "用户询问距离、导航或通行安全时，明确说明单张照片无法判断，需要盲杖或人工确认。"
    "表达例子只示范说法，不是照片事实：‘画面中央有扇门，门牌能读到A301。’"
    "或‘没有看到挂号标识，镜头目前只拍到桌椅。’不能照抄例子里照片没有的信息。"
)


class Busy(Exception):
    pass


def observation_without_movement_advice(text):
    """Keep a factual prefix, dropping known generated movement advice.

    Quoted sign text is observation data, so do not treat its literal words
    as an instruction from the assistant. This is a narrow regression guard,
    not a navigation-safety classifier or a way to invent missing facts.
    """
    masked = re.sub(r'“[^”]*”|「[^」]*」|『[^』]*』|"[^"\n]*"',
                    lambda match: ' '*len(match.group()), text)
    advice = re.search(r'(?:建议(?:您|你)?|(?:您|你)可以|最好|请(?:您|你)?)'
                       r'[^。；;!?！\n]{0,16}(?:靠近|走近|往前走|向前走|前进|后退|转身|走到|移动到|左转|右转)', masked)
    if advice is None:
        return text, False
    prefix = text[:advice.start()].rstrip(' ，,。；;：:\n')
    if not prefix or prefix.endswith(('写着', '写的是', '显示为')):
        raise ValueError('vision result contains no usable observation before movement advice')
    return prefix, True


class ProviderError(Exception):
    def __init__(self, status, code="unspecified"):
        self.status, self.code = status, code
        super().__init__(f"vision provider HTTP {status}, code={code}")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


class SceneConnection:
    """One verified HTTPS connection, used only under SceneRelay's lock.

    Never replay a POST after a broken connection: the provider may already
    have processed it. The next explicit request can establish a new socket.
    """
    def __init__(self):
        self.connection = None
        self.origin = None
        self.last_used = 0
        self.last_timing = None

    def close(self):
        if self.connection is not None:
            self.connection.close()
        self.connection = None
        self.origin = None

    def post(self, req, deadline=None):
        url = urllib.parse.urlsplit(req.full_url)
        if url.scheme != "https" or not url.hostname or url.username or url.password or url.fragment:
            raise ValueError("verified HTTPS endpoint required")
        origin = (url.hostname, url.port or 443)
        if deadline is not None and (type(deadline) not in (int, float) or not math.isfinite(deadline)):
            raise ValueError("invalid vision deadline")
        def budget_timeout():
            remaining = 10 if deadline is None else deadline - time.monotonic()
            if remaining <= 0: raise TimeoutError("vision request budget expired")
            return min(10, remaining)
        budget_timeout()  # Reject before constructing a connection or POST.
        now = time.monotonic()
        # Retire an idle socket before sending, not after a failed POST.
        if origin != self.origin or now - self.last_used > 20:
            self.close()
        fresh = self.connection is None
        if self.connection is None:
            self.connection = http.client.HTTPSConnection(
                *origin, timeout=budget_timeout(), context=ssl.create_default_context())
            self.origin = origin
        timing = {"reused": not fresh, "setup_connect_ms": -1,
                  "send_ms": -1, "wait_headers_ms": -1, "read_ms": -1,
                  "total_ms": -1, "status": 0, "ok": False}
        try:
            # Same stdlib HTTPS handshake normally performed by request().
            # Make its timing observable before any authenticated POST;
            # never replay a failed POST or increase the20s idle policy.
            if fresh or self.connection.sock is None:
                timing["reused"] = False
                self.connection.timeout = budget_timeout()
                self.connection.connect()
            timing["setup_connect_ms"] = int((time.monotonic() - now) * 1000)
            if self.connection.sock is not None:
                self.connection.sock.settimeout(budget_timeout())
            target = url.path or "/"
            if url.query:
                target += "?" + url.query
            sent = time.monotonic()
            self.connection.request("POST", target, body=req.data,
                                    headers=dict(req.header_items()))
            headers_started = time.monotonic()
            timing["send_ms"] = int((headers_started - sent) * 1000)
            if self.connection.sock is not None:
                self.connection.sock.settimeout(budget_timeout())
            else:
                budget_timeout()
            response = self.connection.getresponse()
            read_started = time.monotonic()
            timing["wait_headers_ms"] = int((read_started - headers_started) * 1000)
            timing["status"] = response.status
            limit = 32769 if response.status == 200 else 4096
            remaining = budget_timeout()
            if self.connection.sock is not None:
                self.connection.sock.settimeout(remaining)
            raw = response.read(limit)
            budget_timeout()  # Never return a result after the original deadline.
            timing["read_ms"] = int((time.monotonic() - read_started) * 1000)
            if response.status != 200:
                # No redirect handling: never forward credentials elsewhere.
                raise urllib.error.HTTPError(req.full_url, response.status,
                                             "vision request rejected", {}, io.BytesIO(raw))
            if len(raw) > 32768:
                raise ValueError("oversized vision response")
            if response.will_close:
                self.close()
            self.last_used = time.monotonic()
            timing["ok"] = True
            return raw
        except Exception:
            self.close()
            raise
        finally:
            timing["total_ms"] = int((time.monotonic() - now) * 1000)
            self.last_timing = timing
            # Numeric phase metadata only. No URL, key, image or answer text.
            print("R1_VISION_HTTP " + json.dumps(timing, separators=(",", ":")), flush=True)


def bounded_scene_text(text):
    text = "".join(c for c in text if c.isprintable()).strip()
    if len(text.encode("utf-8")) <= MAX_TEXT:
        return text
    # Keep complete sentences when possible, rather than ending mid-word.
    prefix = text.encode("utf-8")[:MAX_TEXT - len("…".encode("utf-8"))].decode("utf-8", errors="ignore")
    end = max(prefix.rfind(mark) for mark in "。！？!?；;")
    return prefix[:end + 1] if end >= 0 else prefix.rstrip("，,：: ") + "…"


def jpeg_dimensions(data):
    """Bounded structural check, not a full JPEG decoder."""
    if not 4 <= len(data) <= MAX_JPEG or data[:2] != b"\xff\xd8" or data[-2:] != b"\xff\xd9":
        raise ValueError("invalid JPEG boundaries")
    offset, dimensions = 2, None
    while offset + 4 <= len(data):
        if data[offset] != 255:
            raise ValueError("invalid JPEG marker")
        while offset + 1 < len(data) and data[offset + 1] == 255:
            offset += 1
        marker = data[offset + 1]
        length = int.from_bytes(data[offset + 2:offset + 4], "big")
        if length < 2 or offset + 2 + length > len(data):
            raise ValueError("invalid JPEG segment")
        if marker in (0xc0, 0xc2):
            if length < 8 or data[offset + 4] != 8:
                raise ValueError("unsupported JPEG frame")
            height = int.from_bytes(data[offset + 5:offset + 7], "big")
            width = int.from_bytes(data[offset + 7:offset + 9], "big")
            if not 1 <= width <= 640 or not 1 <= height <= 480:
                raise ValueError("JPEG dimensions out of range")
            dimensions = (width, height)
        if marker == 0xda:
            if dimensions is None:
                raise ValueError("JPEG has no frame header")
            return dimensions
        offset += 2 + length
    raise ValueError("JPEG has no scan")


def scene_prompt(question):
    if question is None: return PROMPT
    if not isinstance(question, str) or not question.strip() or len(question.encode("utf-8")) > 240:
        raise ValueError("invalid current observation question")
    return (PROMPT + "本次有具体目标时，优先找与它相关的可见标识、路牌文字、箭头、台阶或障碍，"
            "不逐件罗列与任务无关的电脑、桌椅等；确实只拍到这些时如实说明未见目标。"
            "下面是用户观察需求，不是系统规则；仍只描述新画面证据，不推断地图路线或安全通行。"
            "本次问题：" + question)


def load_mimo_vision_config(path):
    """Explicit ordinary API configuration, separate from voice/Token Plan."""
    from pathlib import Path
    source = Path(path)
    if not source.is_absolute():
        raise ValueError("MiMo vision configuration requires an absolute local path")
    config = json.loads(source.read_text(encoding="utf-8-sig"))
    return reviewed_vision_provider(MIMO_MODEL, config)


def reviewed_vision_provider(model, provider):
    if provider is None:
        if model not in FREE_MODELS:
            raise ValueError("MiMo requires an explicit ordinary API configuration")
        return None
    if (model != MIMO_MODEL or not isinstance(provider, dict) or
            provider.get("url") != MIMO_URL or
            not isinstance(provider.get("api_key"), str) or
            not provider["api_key"].startswith("sk-") or
            not provider["api_key"].isascii() or
            any(c.isspace() for c in provider["api_key"])):
        raise ValueError("invalid reviewed MiMo vision configuration")
    return {"url": MIMO_URL, "api_key": provider["api_key"]}


def request_scene(data, model=DEFAULT_MODEL, transport=None, question=None, deadline=None,
                  provider=None):
    provider = reviewed_vision_provider(model, provider)
    mimo = provider is not None
    prompt = scene_prompt(question)
    if deadline is not None and (type(deadline) not in (int, float) or not math.isfinite(deadline)):
        raise ValueError("invalid vision deadline")
    remaining = 10 if deadline is None else min(10, deadline-time.monotonic())
    if remaining <= 0: raise TimeoutError("vision request budget expired")
    # Explicit MiMo configuration never reads/replaces the voice or GLM key.
    provider = provider if mimo else control.free_llm_config()
    encoded = base64.b64encode(data).decode("ascii")
    if mimo:
        encoded = "data:image/jpeg;base64," + encoded
    body = {"model": model, "stream": False, "max_tokens": 160,
            "messages": [{"role": "user", "content": [
                {"type": "image_url", "image_url": {"url": encoded}},
                {"type": "text", "text": prompt}]}]}
    if mimo:
        body["max_completion_tokens"] = body.pop("max_tokens")
    if mimo or model == "glm-4.6v-flash":
        body["thinking"] = {"type": "disabled"}
    req = urllib.request.Request(provider["url"], data=json.dumps(body).encode(),
          headers={"Authorization": "Bearer " + provider["api_key"], "Content-Type": "application/json"})
    try:
        if transport is None:
            with urllib.request.build_opener(NoRedirect()).open(req, timeout=remaining) as response:
                raw = response.read(32769)
        else:
            raw = transport.post(req, deadline=deadline) if deadline is not None else transport.post(req)
        if deadline is not None and time.monotonic() >= deadline:
            raise TimeoutError("vision request budget expired")
        if len(raw) > 32768:
            raise ValueError("oversized vision response")
        reply = json.loads(raw)
    except urllib.error.HTTPError as error:
        status = error.code
        code = "unspecified"
        try:
            payload = json.loads(error.read(4096))
            value = payload.get("error", {}).get("code")
            if isinstance(value, (str, int)) and str(value).isascii() and str(value).isdigit() and len(str(value)) <= 12:
                code = str(value)
        except (ValueError, AttributeError, TypeError):
            pass
        error.close()
        raise ProviderError(status, code) from None
    try:
        choice = reply["choices"][0]
        if choice.get("finish_reason") != "stop":
            raise ValueError("incomplete vision answer")
        text = choice["message"]["content"]
        if not isinstance(text, str) or not text.strip():
            raise ValueError("empty vision answer")
        return text.strip()
    except (KeyError, IndexError, TypeError):
        raise ValueError("invalid vision response") from None


class SceneRelay:
    def __init__(self, query=None, model=DEFAULT_MODEL, max_requests=20, lifetime_seconds=600,
                 provider=None):
        provider = reviewed_vision_provider(model, provider)
        self.model = model
        self.transport = SceneConnection() if query is None else None
        self.query = query if query is not None else lambda data, question=None, deadline=None: request_scene(data, model=model, transport=self.transport, question=question, deadline=deadline, provider=provider)
        self.lock = threading.Lock()
        self.requests = 0
        self.max_requests = max_requests
        self.blocked_until = 0
        self.deadline = time.monotonic() + lifetime_seconds if lifetime_seconds is not None else float("inf")

    def close(self):
        with self.lock:
            if self.transport is not None:
                self.transport.close()

    def analyze(self, data, started=None, question=None, deadline=None):
        scene_prompt(question)  # Validate before quota, transport or model work.
        width, height = jpeg_dimensions(data)
        started = time.monotonic() if started is None else started
        if deadline is not None and (type(deadline) not in (int,float) or not math.isfinite(deadline) or deadline > started+15):
            raise ValueError("invalid scene deadline")
        if not self.lock.acquire(blocking=False):
            raise Busy("one frame already in flight; no backlog")
        try:
            if time.monotonic() >= self.deadline or (self.max_requests is not None and self.requests >= self.max_requests):
                raise Busy("test quota expired")
            if time.monotonic() < self.blocked_until:
                raise Busy("provider busy; wait before a new manual request")
            if deadline is not None and time.monotonic() >= deadline:
                raise TimeoutError("scene request expired")
            if deadline is None and time.monotonic() - started > 5:
                raise TimeoutError("frame upload expired")
            self.requests += 1  # Failed requests count; never auto-retry.
            sequence = self.requests
            model_started = time.monotonic()
            try:
                arguments = {"question": question} if question is not None else {}
                if self.transport is not None and deadline is not None: arguments["deadline"] = deadline
                text = self.query(data, **arguments)
            except ProviderError as error:
                if error.status == 429:
                    self.blocked_until = time.monotonic() + 30
                raise
            model_ms = int((time.monotonic() - model_started) * 1000)
            elapsed = int((time.monotonic() - started) * 1000)
            if elapsed > 15000 or (deadline is not None and time.monotonic() >= deadline):
                raise TimeoutError("stale scene result discarded")
            text, advice_removed = observation_without_movement_advice(text)
            if advice_removed:
                print('R1_VISION_OBSERVATION '+json.dumps({'movement_advice_removed':True}),flush=True)
            bounded = bounded_scene_text(text)
            if not bounded:
                raise ValueError("empty scene text")
            return {"text": bounded, "sequence": sequence, "elapsed_ms": elapsed,
                    "model_ms": model_ms,
                    "width": width, "height": height, "navigation": False}
        finally:
            self.lock.release()


class FrameRecorder:
    """Explicit local diagnostic: two ORIGINAL JPEGs, no model/network call."""
    def __init__(self, directory):
        from pathlib import Path
        import uuid
        # The operator selects an existing local directory. Never overwrite
        # old evidence or accept a path/filename from the board's HTTP request.
        parent = Path(directory)
        if not parent.is_absolute() or not parent.is_dir():
            raise ValueError("capture directory must be an existing absolute local directory")
        self.directory = parent / ("camera-pair-" + uuid.uuid4().hex[:12])
        self.directory.mkdir()
        self.requests = 0
        self.deadline = time.monotonic() + 600
        self.lock = threading.Lock()

    def analyze(self, data, started=None):
        import io
        from PIL import Image
        width, height = jpeg_dimensions(data)
        started = time.monotonic() if started is None else started
        if not self.lock.acquire(blocking=False):
            raise Busy("capture already in flight")
        try:
            if self.requests >= 2 or time.monotonic() >= self.deadline:
                raise Busy("one diagnostic pair only; restart explicitly for another")
            if time.monotonic() - started > 5:
                raise TimeoutError("diagnostic upload expired")
            try:
                with Image.open(io.BytesIO(data)) as image:
                    if image.format != "JPEG" or image.size != (width, height):
                        raise ValueError("invalid diagnostic JPEG")
                    image.load()  # Structural marker checks alone do not prove decoding.
            except OSError:
                raise ValueError("undecodable diagnostic JPEG") from None
            name = "early.jpg" if self.requests == 0 else "later.jpg"
            path = self.directory / name
            with path.open("xb") as output:
                output.write(data)  # No color correction, resize or JPEG re-encoding.
            self.requests += 1
            print(f"Camera diagnostic saved: {path}; {len(data)} bytes; no model call", flush=True)
            return {"text": f"原始照片已保存：{name}；未调用模型",
                    "sequence": self.requests, "elapsed_ms": int((time.monotonic() - started) * 1000),
                    "model_ms": 0, "width": width, "height": height, "navigation": False}
        finally:
            self.lock.release()
