"""Shared Agora VAD configuration, using the published REST field schema."""
# SPDX-License-Identifier: Apache-2.0


def vad_turn_detection(prefix_padding_ms=800, silence_duration_ms=640):
    # Return a fresh object: per-session overrides must not alter other modes.
    return {"mode": "default", "config": {
        "speech_threshold": 0.5,
        "start_of_speech": {"mode": "vad", "vad_config": {
            "interrupt_duration_ms": 160, "prefix_padding_ms": prefix_padding_ms}},
        "end_of_speech": {"mode": "vad", "vad_config": {
            "silence_duration_ms": silence_duration_ms}}}}
