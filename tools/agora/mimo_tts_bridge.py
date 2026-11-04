"""MiMo SSE -> OpenAI-compatible chunked PCM, loopback only by default.

Protocol sources: MiMo speech-synthesis-v2.5; Shengwang custom-tts-agent.
No paid fallback, playback, microphone, Agora join, or automatic public tunnel.
Credentials and the manually checked free-price window live outside Git.
"""
import argparse
import base64
from datetime import datetime, timezone
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import signal
import threading
import time
import urllib.error
import urllib.request
from urllib.parse import urlparse
from turn_detection import vad_turn_detection

ROOT = Path(os.environ.get("R1_LOCAL_ROOT", Path(__file__).resolve().parents[4]))
ENDPOINT = "https://api.xiaomimimo.com/v1/chat/completions"
MODEL = "mimo-v2.5-tts"
RATE = 24000
MAX_PCM = RATE * 2 * 30
MAX_SECONDS = 30


def validate_config(cfg, now=None):
    now = time.time() if now is None else now
    key = cfg.get("api_key", "")
    if not isinstance(key, str) or not key.startswith("sk-") or len(key) < 20:
        raise ValueError("ordinary MiMo API key required; Token Plan is not used")
    if cfg.get("model") != MODEL or cfg.get("voice") not in ("冰糖", "茉莉", "苏打", "白桦", "mimo_default"):
        raise ValueError("unsupported model or voice; no fallback")
    try:
        checked = datetime.fromisoformat(cfg["free_verified_until"])
        if checked.tzinfo is None: raise ValueError()
        until = checked.timestamp()
    except (KeyError, TypeError, ValueError):
        raise ValueError("dated free-price confirmation required") from None
    if cfg.get("free_only") is not True or not now < until <= now + 86400:
        raise ValueError("free-price confirmation expired or exceeds 24 hours; recheck pricing")
    if type(cfg.get("max_requests")) is not int or not 1 <= cfg["max_requests"] <= 20:
        raise ValueError("bounded request limit required")
    return cfg


def load_config():
    return validate_config(json.loads((ROOT / "config" / "mimo-tts.local.json").read_text(encoding="utf-8-sig")))


def bridge_key():
    path = ROOT / "config" / "mimo-tts-bridge-key.txt"
    if not path.exists():
        # Never overwrite another process's newly created key.
        try:
            with path.open("x", encoding="ascii") as output:
                output.write(secrets.token_hex(32))
        except FileExistsError:
            pass
    key = path.read_text(encoding="ascii").strip()
    if len(key) != 64 or any(c not in "0123456789abcdef" for c in key):
        raise ValueError("invalid private bridge key")
    return key


def agora_voice_config():
    cfg = load_config()
    url = urlparse(cfg.get("public_url", ""))
    if (url.scheme != "https" or not url.hostname or url.username or url.password or
            url.path != "/v1/audio/speech" or url.query or url.fragment):
        raise ValueError("authenticated public HTTPS TTS endpoint is not configured; cloud join blocked")
    quota = cfg.get("agora_free_minutes", {})
    if any(type(quota.get(k)) is not int or quota[k] < 20 for k in ("rtc", "agent", "asr")):
        raise ValueError("recent free-quota confirmation required; cloud join blocked")
    return {
        "asr": {"vendor": "fengming", "language": "zh-CN"},
        "tts": {"vendor": "generic_http", "url": url.geturl(),
                "headers": {"Authorization": "Bearer " + bridge_key()},
                "params": {"model": MODEL, "voice": cfg["voice"], "speed": 1.0,
                           "sample_rate": RATE, "response_format": "pcm"}},
        "turn_detection": vad_turn_detection(prefix_padding_ms=300, silence_duration_ms=480),
    }


def validate_input(body, cfg):
    if not isinstance(body, dict): raise ValueError("JSON object required")
    text = body.get("input")
    if not isinstance(text, str) or not text.strip() or len(text) > 500:
        raise ValueError("input must contain 1..500 characters")
    if (body.get("model", MODEL) != MODEL or body.get("voice", cfg["voice"]) != cfg["voice"] or
            body.get("response_format", "pcm") != "pcm" or
            type(body.get("sample_rate", RATE)) is not int or body.get("sample_rate", RATE) != RATE or
            type(body.get("speed", 1)) not in (int, float) or body.get("speed", 1) != 1):
        raise ValueError("only configured voice, PCM16LE mono 24000 Hz and speed 1 are supported")
    instruction = body.get("instruction", "")
    if not isinstance(instruction, str) or len(instruction) > 200:
        raise ValueError("invalid instruction")
    return text, instruction


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        raise ValueError("upstream redirect refused")


def read_pcm(response, started):
    """Bounded SSE parser; fail on truncation, errors or unaligned PCM."""
    total = pcm_size = event_size = 0
    event, tail = [], b""
    while True:
        if time.monotonic() - started > MAX_SECONDS:
            raise TimeoutError("TTS exceeded time limit")
        line = response.readline(262145)
        total += len(line)
        if len(line) > 262144 or total > 4 * MAX_PCM:
            raise ValueError("oversized TTS stream")
        if not line: raise ValueError("TTS stream ended without DONE")
        line = line.decode("utf-8").rstrip("\r\n")
        if line.startswith("data:"):
            event.append(line[5:].lstrip(" "))
            event_size += len(line)
            if event_size > 262144: raise ValueError("oversized SSE event")
        elif not line and event:
            data = "\n".join(event)
            event, event_size = [], 0
            if data == "[DONE]":
                if pcm_size == 0 or tail: raise ValueError("empty or unaligned PCM")
                return
            message = json.loads(data)
            if not isinstance(message, dict) or "error" in message:
                raise ValueError("upstream error event")
            choices = message.get("choices", [])
            if not isinstance(choices, list): raise ValueError("invalid choices")
            if not choices: continue
            if not isinstance(choices[0], dict): raise ValueError("invalid choice")
            delta = choices[0].get("delta", {})
            if not isinstance(delta, dict): raise ValueError("invalid delta")
            audio = delta.get("audio")
            if audio is None: continue
            if not isinstance(audio, dict) or not isinstance(audio.get("data"), str):
                raise ValueError("invalid audio data")
            raw = base64.b64decode(audio["data"], validate=True)
            pcm_size += len(raw)
            if pcm_size > MAX_PCM: raise ValueError("TTS audio exceeds 30 seconds")
            raw = tail + raw
            tail = raw[-1:] if len(raw) % 2 else b""
            aligned = raw[:len(raw) - len(tail)]
            if aligned: yield aligned


def synthesize(cfg, text, instruction=""):
    validate_config(cfg)
    messages = ([{"role": "user", "content": instruction}] if instruction else [])
    messages.append({"role": "assistant", "content": text})
    payload = {"model": MODEL, "messages": messages, "stream": True,
               "audio": {"format": "pcm16", "voice": cfg["voice"]}}
    req = urllib.request.Request(ENDPOINT, data=json.dumps(payload).encode(),
          headers={"Authorization": "Bearer " + cfg["api_key"], "Content-Type": "application/json",
                   "Accept": "text/event-stream"})
    started = time.monotonic()
    try:
        with urllib.request.build_opener(NoRedirect()).open(req, timeout=10) as response:
            if "text/event-stream" not in response.headers.get("Content-Type", "").lower():
                raise ValueError("MiMo did not return SSE")
            yield from read_pcm(response, started)
    except urllib.error.HTTPError as error:
        status = error.code
        # Keep only a machine error code and a known billing classification.
        # Provider prose may echo keys/prompts, so never copy it to logs.
        reason = "unspecified"
        try:
            obj = json.loads(error.read(8193))
            item = obj.get("error", {}) if isinstance(obj, dict) else {}
            if isinstance(item, dict):
                code = item.get("code")
                if type(code) is int or code in ("insufficient_balance", "insufficient_quota", "invalid_api_key", "rate_limit_exceeded"):
                    reason = str(code)
                message = str(item.get("message", "")).lower()
                if "insufficient" in message and ("balance" in message or "quota" in message):
                    reason = "insufficient_balance_or_quota"
        except (ValueError, OSError):
            pass
        error.close()
        raise RuntimeError("MiMo TTS HTTP " + str(status) + "; no fallback; reason=" + reason) from None
    except urllib.error.URLError:
        raise RuntimeError("MiMo TTS network failure; no fallback") from None


class Bridge:
    def __init__(self, cfg, source=synthesize):
        self.cfg, self.source = validate_config(cfg), source
        self.lock = threading.Lock()
        self.requests = 0

    def acquire(self):
        if not self.lock.acquire(blocking=False): return False
        try:
            validate_config(self.cfg)
            if self.requests >= self.cfg["max_requests"]:
                raise ValueError("TTS request limit reached; stop and review usage")
            self.requests += 1
            return True
        except Exception:
            self.lock.release()
            raise


def handler(bridge, key):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        def log_message(self, *args): pass
        def reply(self, status):
            raw = json.dumps({"error": {"message": "TTS request rejected; no paid fallback",
                                        "type": "tts_bridge_error", "code": str(status)}}).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(raw)
        def do_POST(self):
            self.close_connection = True
            stream, acquired, sent = None, False, False
            started = time.monotonic()
            size = chunks = 0
            try:
                if not hmac.compare_digest(self.headers.get("Authorization", "").encode(), ("Bearer " + key).encode()):
                    self.reply(401)
                    return
                if self.path != "/v1/audio/speech":
                    self.reply(404)
                    return
                length = int(self.headers.get("Content-Length", "0"))
                if not 1 <= length <= 8192 or self.headers.get("Transfer-Encoding"):
                    self.reply(413)
                    return
                if self.headers.get_content_type() != "application/json":
                    self.reply(415)
                    return
                raw = self.rfile.read(length)
                if len(raw) != length: raise ValueError("truncated request")
                text, instruction = validate_input(json.loads(raw), bridge.cfg)
                acquired = bridge.acquire()
                if not acquired:
                    self.reply(429)
                    return
                stream = bridge.source(bridge.cfg, text, instruction)
                first = next(stream)  # Upstream failure before headers is HTTP 502, not false success.
                first_ms = round((time.monotonic() - started) * 1000)
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Transfer-Encoding", "chunked")
                self.send_header("Connection", "close")
                self.send_header("X-Audio-Sample-Rate", str(RATE))
                self.end_headers()
                sent = True
                def write_chunk(pcm):
                    nonlocal size, chunks
                    if not pcm or len(pcm) % 2: raise ValueError("invalid PCM chunk")
                    size += len(pcm)
                    if size > MAX_PCM or time.monotonic() - started > MAX_SECONDS:
                        raise TimeoutError("bridge stream limit")
                    self.wfile.write(f"{len(pcm):X}\r\n".encode() + pcm + b"\r\n")
                    self.wfile.flush()
                    chunks += 1
                write_chunk(first)
                for pcm in stream: write_chunk(pcm)
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
                print(f"TTS PASS bytes={size} chunks={chunks} first_ms={first_ms} total_ms={round((time.monotonic()-started)*1000)}", flush=True)
            except (ValueError, UnicodeError) as error:
                if not sent: self.reply(502 if acquired else 400)
                print("TTS rejected: " + type(error).__name__, flush=True)
            except Exception as error:
                if not sent:
                    try: self.reply(502)
                    except OSError: pass
                # After partial PCM, omit terminal chunk: consumer sees truncation.
                print("TTS failed: " + type(error).__name__, flush=True)
            finally:
                try:
                    if stream is not None: stream.close()
                finally:
                    if acquired: bridge.lock.release()
    return Handler


class Server(ThreadingHTTPServer):
    daemon_threads = False
    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(10)
        return connection, address


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("probe", "serve"))
    parser.add_argument("--text", default="你好，我是R1语音助手。")
    parser.add_argument("--port", type=int, default=8767)
    parser.add_argument("--duration", type=int, default=300)
    args = parser.parse_args()
    cfg = load_config()
    if args.action == "probe":
        text, _ = validate_input({"input": args.text}, cfg)
        started = time.monotonic()
        size = chunks = 0
        first = None
        digest = hashlib.sha256()
        for pcm in synthesize(cfg, text):
            if first is None: first = round((time.monotonic() - started) * 1000)
            size += len(pcm)
            chunks += 1
            digest.update(pcm)
        print(f"MiMo TTS PASS rate={RATE} bits=16 mono bytes={size} chunks={chunks} first_ms={first} total_ms={round((time.monotonic()-started)*1000)} audio_ms={size*1000//(RATE*2)} sha256={digest.hexdigest()}")
        return
    if not 1 <= args.port <= 65535 or not 1 <= args.duration <= 600:
        parser.error("port 1..65535 and duration 1..600 required")
    bridge = Bridge(cfg)
    server = Server(("127.0.0.1", args.port), handler(bridge, bridge_key()))
    server.timeout = 0.5
    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    deadline = time.monotonic() + args.duration
    print(f"MiMo TTS bridge on loopback port {args.port}; no public exposure or Agora join", flush=True)
    try:
        while not stop.is_set() and time.monotonic() < deadline:
            server.handle_request()
    finally:
        server.server_close()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        # Never show provider bodies, prompts, Authorization or key-bearing URLs.
        print("MiMo TTS stopped: " + type(error).__name__ + (": " + str(error) if isinstance(error, (ValueError, RuntimeError)) else ""), flush=True)
        raise SystemExit(1)
