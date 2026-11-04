"""Optional LAN-assisted wake: official sherpa-onnx streaming ASR, RAM only.

Recognizer feeding/endpoint reset follows sherpa-onnx v1.13.8's
online-decode-files.py and streaming_server.py (Apache-2.0). This transport
does not start an RTC agent. Only an explicit firmware wake-start may hand
the released Audio owner to the existing conversation service.
"""
import hashlib
import json
from pathlib import Path
import re
import secrets
import threading
import time
import unicodedata


def matches_wake(text):
    # Keep the official complete greeting, accepting ASR spaces/punctuation.
    # Never accept a vague "vela", a Chinese homophone or openvelax.
    normalized = "".join(c for c in unicodedata.normalize("NFKC", text).casefold()
                         if c.isalnum())
    return re.search(r"(?<![a-z])(?:你好|hello)openvela(?![a-z])", normalized) is not None


class SherpaRecognizer:
    def __init__(self, model_root):
        import numpy as np
        import sherpa_onnx
        root = Path(model_root)
        manifest = json.loads((root / "verified-inputs.json").read_text())
        lock = json.loads(Path(__file__).with_name("local-asr-model.json").read_text())
        if (manifest.get("repository") != lock["repository"] or
                manifest.get("revision") != lock["revision"]):
            raise ValueError("unexpected local ASR model snapshot")
        records = {item["path"]: item["sha256"] for item in manifest["files"]}
        for item in lock["files"]:
            name, digest = item["path"], item["sha256"]
            if records.get(name) != digest or hashlib.sha256((root / name).read_bytes()).hexdigest() != digest:
                raise ValueError("local ASR model integrity check failed: " + name)
        if hashlib.sha256((root / "bpe.vocab").read_bytes()).hexdigest() != lock["generated_bpe_vocab_sha256"]:
            raise ValueError("unexpected generated ASR BPE vocabulary")
        self.np = np
        self.recognizer = sherpa_onnx.OnlineRecognizer.from_transducer(
            tokens=str(root / "tokens.txt"),
            encoder=str(root / "encoder-epoch-99-avg-1.int8.onnx"),
            decoder=str(root / "decoder-epoch-99-avg-1.int8.onnx"),
            joiner=str(root / "joiner-epoch-99-avg-1.int8.onnx"),
            num_threads=2, sample_rate=16000, feature_dim=80,
            decoding_method="modified_beam_search", max_active_paths=8, provider="cpu",
            modeling_unit="cjkchar+bpe", bpe_vocab=str(root / "bpe.vocab"),
            hotwords_file=str(Path(__file__).with_name("local-wake-hotwords.txt")), hotwords_score=2.0,
            enable_endpoint_detection=True, rule1_min_trailing_silence=2.4,
            rule2_min_trailing_silence=1.2, rule3_min_utterance_length=20)

    def create(self):
        return self.recognizer.create_stream()

    def feed(self, stream, pcm):
        samples = self.np.frombuffer(pcm, dtype="<i2").astype(self.np.float32) / 32768
        stream.accept_waveform(16000, samples)
        while self.recognizer.is_ready(stream):
            self.recognizer.decode_stream(stream)
        text = self.recognizer.get_result(stream)
        endpoint = self.recognizer.is_endpoint(stream)
        if endpoint:
            self.recognizer.reset(stream)
        return text, endpoint


class LocalWake:
    """One fresh, expiring stream. Sequence/gap errors fail closed; no retry.

    500ms chunks bound board RAM and latency. Real-time pacing and a two-second
    gap bound prevent replaying a buffered conversation into a later wake.
    The ASR firmware additionally bounds PCM buffering to 2s and rejects any
    decision older than 1.5s. Its on-board acoustic KWS policy stays at 800ms.
    """
    def __init__(self, engine, clock=time.monotonic, trace_text=False):
        self.engine, self.clock, self.trace_text = engine, clock, trace_text
        self.lock = threading.Lock()
        self.current = None
        self.closed = False

    def _expire(self):
        if self.current and self.clock() - self.current["last"] > self.current["idle_seconds"]:
            self.current = None

    def start(self, *, streaming=False):
        with self.lock:
            if self.closed: raise ValueError("local wake service closed")
            self._expire()
            if self.current is not None:
                raise BlockingIOError("wake stream already active")
            now = self.clock()
            self.current = dict(id=secrets.token_hex(16), stream=self.engine.create(),
                                seq=0, started=now, last=now, samples=0, matched=False,
                                previous_text="", idle_seconds=3 if streaming else 2)
            return {"stream": self.current["id"], "protocol": 1}

    def feed(self, stream_id, sequence, pcm):
        with self.lock:
            self._expire()
            current = self.current
            if not current or not secrets.compare_digest(stream_id, current["id"]):
                raise TimeoutError("wake stream expired")
            if current["matched"] or sequence != current["seq"]:
                self.current = None
                raise ValueError("wake chunk sequence mismatch")
            if len(pcm) != 16000:
                self.current = None
                raise ValueError("expected 500ms signed little-endian PCM16/16kHz/mono")
            now = self.clock()
            if (current["samples"] + 8000) / 16000 > now - current["started"] + 1:
                self.current = None
                raise ValueError("audio is ahead of live clock")
            started = self.clock()
            try:
                text, endpoint = self.engine.feed(current["stream"], pcm)
            except Exception:
                self.current = None
                raise
            matched = matches_wake(text)
            current.update(seq=sequence + 1, samples=current["samples"] + 8000,
                           last=self.clock(), matched=matched)
            # A very slow CPU may finish after the sender's queue is obsolete.
            if self.clock() - started > 0.8:
                self.current = None
                raise TimeoutError("wake inference exceeded freshness budget")
            text = " ".join(text.split())[:160]
            if self.trace_text and (text != current["previous_text"] or endpoint):
                print("R1_WAKE_TEXT " + json.dumps({"text": text, "matched": matched,
                      "endpoint": endpoint, "audio_ms": current["samples"]//16}, ensure_ascii=False), flush=True)
            current["previous_text"] = text
            return {"stream": stream_id, "seq": sequence, "matched": matched,
                    "text": text, "endpoint": endpoint,
                    "server_ms": max(0, int((self.clock()-started)*1000))}

    def stop(self, stream_id):
        with self.lock:
            if self.current and secrets.compare_digest(stream_id, self.current["id"]):
                self.current = None
            return {"ok": True}

    def reset_decoder(self, stream_id):
        """A streaming result was emitted once; later PCM must be new speech.

        Keep the same connection's sequence/clock ownership. A late positive
        ACK can then be ignored by the board without leaving ASR stuck on an
        old positive result. HTTP's one-shot behavior remains unchanged.
        """
        with self.lock:
            current = self.current
            if not current or not secrets.compare_digest(stream_id, current["id"]):
                raise TimeoutError("wake stream expired")
            current.update(stream=self.engine.create(), matched=False, previous_text="")

    def reap(self):
        with self.lock:
            self._expire()

    def close(self):
        with self.lock:
            self.closed = True
            self.current = None
