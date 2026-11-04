#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Linux host simulations only: no board, microphone, speaker or cloud transport.
set -euo pipefail
project="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
workspace="$(dirname "$project")"
export R1_LOCAL_ROOT="${R1_LOCAL_ROOT:-$HOME/R1-local}"
export AGORA_TOKEN_REFERENCE="${AGORA_TOKEN_REFERENCE:-$R1_LOCAL_ROOT/tools/agora-token-reference}"
python="$workspace/host-venv/bin/python"
if [[ ! -x "$python" ]]; then
  echo 'Missing workspace host-venv. Restore the locked Ubuntu environment first.' >&2
  exit 1
fi
for program in cc ffmpeg; do
  command -v "$program" >/dev/null || { echo "Missing host dependency: $program" >&2; exit 1; }
done
cd "$project/tools/agora"
"$python" -m unittest test_server test_scene_relay test_session_inspect test_stream test_control_interop test_watch_dialogue test_local_asr_wake test_wake_udp test_scene_speech_proxy test_demo_profile test_eval_scene_policy
cd "$project"
bash tools/agora/test_control.sh
"$python" tools/agora/test_camera.py
"$python" tools/agora/test_rpc.py
"$python" tools/agora/test_product_cli.py
bash tools/agora/test_hal.sh
bash tools/agora/test_session.sh
bash tools/xiaozhi/test_audio_service.sh
"$python" tools/bk7258/test_feedback.py
"$python" tools/bk7258/test_power_service.py
"$python" tools/bk7258/test_power_buttons.py
"$python" tools/bk7258/test_soft_off.py
"$python" tools/bk7258/test_power_display.py
"$python" tools/bk7258/test_product_startup.py
"$python" tools/agora/test_wake_service.py
"$python" tools/agora/test_wake_asr.py
"$python" tools/agora/test_wake_stream.py
"$python" tools/agora/test_wake_udp_adapter.py
echo R1_OFFLINE_CHECKS_PASS_NO_HARDWARE_OR_CLOUD
