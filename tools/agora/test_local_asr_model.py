#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual optional ASR engine on public/synthetic WAVs, no live audio or cloud.

Synthetic success is not R1 microphone/human acoustic acceptance.
"""
import argparse
import json
from pathlib import Path
import time
import wave
from local_asr_wake import SherpaRecognizer, matches_wake


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-root", required=True, type=Path)
    parser.add_argument("--positive", nargs="*", default=[], type=Path)
    parser.add_argument("--negative", nargs="*", default=[], type=Path)
    parser.add_argument("--lead-ms", nargs="+", type=int, default=[0],
                        help="test leading silence phases independently of network delivery")
    args = parser.parse_args()
    if any(not 0 <= lead <= 10000 for lead in args.lead_ms):
        parser.error("leading silence must be 0..10000 ms")
    engine = SherpaRecognizer(args.model_root)
    cases = []
    for path, expected in [(p, True) for p in args.positive] + [(p, False) for p in args.negative]:
        with wave.open(str(path), "rb") as audio:
            assert (audio.getframerate(),audio.getnchannels(),audio.getsampwidth()) == (16000,1,2)
            pcm = audio.readframes(audio.getnframes())
        for lead in args.lead_ms:
            cases.append((f"{path.name}:lead{lead}ms", bytes(lead*32)+pcm, expected))
    cases += [("digital-silence", bytes(16000*12), False),
              ("public-mixed-language-0", None, False)]
    for name, pcm, expected in cases:
        if pcm is None:
            with wave.open(str(args.model_root / "test_wavs/0.wav"), "rb") as audio:
                pcm = audio.readframes(audio.getnframes())
        stream = engine.create()
        texts, costs = [], []
        for offset in range(0,len(pcm)+64000,16000):
            started = time.perf_counter()
            text, endpoint = engine.feed(stream,pcm[offset:offset+16000].ljust(16000,b"\0"))
            costs.append((time.perf_counter()-started)*1000)
            if text and (not texts or text != texts[-1]): texts.append(text)
        hit = any(matches_wake(text) for text in texts)
        print(json.dumps({"case":name,"texts":texts,"matched":hit,
                          "chunk_max_ms":round(max(costs),1)},ensure_ascii=False),flush=True)
        assert hit == expected, (name, expected, texts)
        assert max(costs) < 800, "ASR CPU exceeded the live freshness budget"
    print("LOCAL_ASR_REAL_MODEL_FIXTURES_PASS; synthetic/public audio only; no human acceptance")


if __name__ == "__main__":
    main()
