"""Bounded LAN PCM datagrams; reuse LocalWake and its official ASR engine.

20ms current + previous PCM frames fit below the Ethernet MTU (1320 bytes).
The previous frame repairs one lost datagram. Up to 80ms of unavailable PCM
is zero-filled to preserve its sample clock; larger gaps reset the decoder.
HMAC uses Python's standard library / board mbedTLS.
No recorder, cloud operation, unbounded work queue or per-packet thread.
"""
import hashlib
from collections import deque
import hmac
import json
import socket
import struct
import threading
import time

PCM_BYTES = 640
PACKET_BYTES = 1320
ACK_BYTES = 41
MAX_CONCEAL_FRAMES = 4  # 80ms, after recovering the previous frame
ACK_HOLD_SECONDS = .04


def signed(key, body):
    return body + hmac.digest(key, body, hashlib.sha256)[:16]


class WakeUDP:
    def __init__(self, address, key, wake, clock=time.monotonic, trace=False):
        self.key, self.wake, self.clock = bytes.fromhex(key), wake, clock
        self.trace = trace
        self.lock = threading.Lock()
        self.current = None
        self.pending = deque(maxlen=8)  # ACK metadata only, never PCM
        self.closed = threading.Event()
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            # At 66 KiB/s this holds < 0.5s of PCM redundancy on Windows.
            # Per-frame clock checks additionally reject old queued packets.
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32768)
            self.socket.bind(address)
            self.socket.settimeout(.01)
        except Exception:
            self.socket.close()
            raise
        self.address = self.socket.getsockname()
        self.thread = threading.Thread(target=self._run, name="r1-wake-udp", daemon=True)
        self.thread.start()

    def start(self, peer_ip):
        with self.lock:
            if self.closed.is_set():
                raise ValueError("UDP wake closed")
            self._expire()
            result = self.wake.start(streaming=True)
            self.current = dict(id=result["stream"], nonce=bytes.fromhex(result["stream"]),
                                peer_ip=peer_ip, peer=None, expected=0, chunk=0,
                                pcm=bytearray(), started=self.clock(), last=self.clock(),
                                ack=None, repeats=0, repaired=0, gaps=0, frames=0,
                                concealed=0, short_gaps=0, inference_max_ms=0,
                                last_ack_sent=None, ack_send_gap_max_ms=0,
                                packet_gap_max_ms=0, age_rejected=0)
            return {**result, "protocol": 3, "udp_port": self.address[1]}

    def stop(self, stream):
        with self.lock:
            if self.current and hmac.compare_digest(stream, self.current["id"]):
                self._clear()
            else:
                self.wake.stop(stream)
        return {"ok": True}

    def _clear(self, reason="stop"):
        self.pending.clear()
        if self.current:
            if self.trace:
                c = self.current
                # Fixed numeric metadata only; no stream/peer/key/PCM/text.
                print('R1_WAKE_UDP ' + json.dumps(dict(
                    reason=reason, elapsed_ms=round((self.clock()-c['started'])*1000),
                    frames=c['frames'], chunks=c['chunk'], age_rejected=c['age_rejected'],
                    repaired=c['repaired'], gaps=c['gaps'],
                    packet_gap_max_ms=c['packet_gap_max_ms'],
                    inference_max_ms=c['inference_max_ms'],
                    ack_send_gap_max_ms=c['ack_send_gap_max_ms'],
                    last_packet_age_ms=round((self.clock()-c['last'])*1000))), flush=True)
            self.wake.stop(self.current["id"])
            self.current["pcm"].clear()
            self.current = None

    def _expire(self):
        if self.current and self.clock() - self.current["last"] > 3:
            self._clear('idle-expired')

    def _accept(self, raw, peer):
        """Called only by the one receiver; deterministic unit tests use it too."""
        with self.lock:
            self._expire()
            c = self.current
            if (not c or len(raw) != PACKET_BYTES or raw[:4] != b"R1U1" or
                    peer[0] != c["peer_ip"] or (c["peer"] and peer != c["peer"]) or
                    not hmac.compare_digest(raw[4:20], c["nonce"]) or
                    not hmac.compare_digest(raw[-16:], hmac.digest(self.key, raw[:-16], "sha256")[:16])):
                return None
            sequence = struct.unpack_from("!I", raw, 20)[0]
            age = self.clock() - c["started"] - sequence * .02
            if sequence < c["expected"]:
                return None
            if not -1 <= age <= 1.5:
                c['age_rejected'] += 1
                return None
            c["peer"] = peer
            c['packet_gap_max_ms'] = max(c['packet_gap_max_ms'], round((self.clock()-c['last'])*1000))
            frames = []
            if sequence > c["expected"]:
                missing = sequence - c["expected"] - 1
                if missing > MAX_CONCEAL_FRAMES:
                    self.wake.reset_decoder(c["id"])
                    c["pcm"].clear()
                    c["ack"], c["repeats"] = None, 0
                    self.pending.clear()
                    c["gaps"] += 1
                elif missing:
                    # Keep the actual missing duration in the waveform. Do
                    # not concatenate the words on either side or invent
                    # speech from repeated PCM. Short Wi-Fi bursts must not
                    # erase the whole utterance on every 40-80ms dropout.
                    frames.extend((i, bytes(PCM_BYTES)) for i in range(c["expected"], sequence - 1))
                    c["concealed"] += missing
                    c["short_gaps"] += 1
                frames.append((sequence - 1, raw[24:664]))
                c["repaired"] += 1
            frames.append((sequence, raw[664:1304]))
            for index, pcm in frames:
                c["pcm"].extend(pcm)
                c["frames"] += 1
                if len(c["pcm"]) == 16000:
                    result = self.wake.feed(c["id"], c["chunk"], bytes(c["pcm"]))
                    c['inference_max_ms'] = max(c['inference_max_ms'], result['server_ms'])
                    c["pcm"].clear()
                    c["chunk"] += 1
                    c["ack"] = signed(self.key, b"R1K1" + c["nonce"] +
                                      struct.pack("!IB", index, result["matched"]))
                    c["repeats"] = 3
                    if result["matched"]:
                        self.wake.reset_decoder(c["id"])
            c["expected"], c["last"] = sequence + 1, self.clock()
            if c["repeats"]:
                c["repeats"] -= 1
                return c["ack"]
            return None

    def _schedule(self, reply, peer):
        with self.lock:
            c = self.current
            if c and reply[4:20] == c["nonce"] and peer == c["peer"]:
                # Current firmware's capture grid stamps the first frame
                # 20ms after its callback, and consumes an ACK sequence even
                # when rejecting that future timestamp. Allow two frame
                # periods before emission; the board's 1.5s age check still
                # applies. Only ACKs wait here, not recording or recognition.
                self.pending.append((self.clock() + ACK_HOLD_SECONDS, reply, peer))

    def _send_due(self):
        with self.lock:
            ready = []
            while self.pending and self.pending[0][0] <= self.clock():
                _, raw, peer = self.pending.popleft()
                if self.current and raw[4:20] == self.current["nonce"] and peer == self.current["peer"]:
                    ready.append((raw, peer))
        for raw, peer in ready:
            self.socket.sendto(raw, peer)
            with self.lock:
                c = self.current
                if c and raw[4:20] == c['nonce'] and peer == c['peer']:
                    sent = self.clock()
                    if c['last_ack_sent'] is not None:
                        c['ack_send_gap_max_ms'] = max(c['ack_send_gap_max_ms'], round((sent-c['last_ack_sent'])*1000))
                    c['last_ack_sent'] = sent

    def _run(self):
        while not self.closed.is_set():
            try:
                # One extra byte detects oversized/truncated datagrams.
                raw = None
                try:
                    raw, peer = self.socket.recvfrom(PACKET_BYTES + 1)
                except socket.timeout:
                    with self.lock:
                        self._expire()
                # socket.timeout aliases TimeoutError in Python. Keep ASR
                # freshness failures OUTSIDE the recv timeout handler: they
                # must clear PCM/decoder ownership, not look like an idle
                # socket and leave a full chunk accumulating beyond16KiB.
                if raw is not None:
                    reply = self._accept(raw, peer)
                    if reply is not None:
                        self._schedule(reply, peer)
                self._send_due()
            except (ConnectionResetError, ConnectionRefusedError):
                # A delayed ACK can reach a board socket that has already
                # closed after stop/wake handoff. Windows reports that peer's
                # ICMP error on this shared, unconnected UDP socket. It does
                # not mean the listener failed, and must not kill a later
                # stream. The authenticated owner/idle timers remain intact.
                continue
            except (ValueError, TimeoutError, RuntimeError) as error:
                with self.lock:
                    reasons={'audio is ahead of live clock':'audio-ahead',
                             'wake inference exceeded freshness budget':'inference-budget',
                             'wake stream expired':'asr-expired',
                             'wake chunk sequence mismatch':'sequence-mismatch'}
                    self._clear(reasons.get(str(error),'asr-failure'))
            except OSError:
                if not self.closed.is_set():
                    with self.lock:
                        self._clear('socket-failure')
                    # Avoid a busy loop on a failed socket.
                    self.closed.set()

    def close(self):
        self.closed.set()
        self.thread.join(timeout=2)
        self.socket.close()
        with self.lock:
            self._clear('shutdown')
