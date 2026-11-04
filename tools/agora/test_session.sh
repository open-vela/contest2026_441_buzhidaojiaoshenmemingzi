#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
project=$(cd "$(dirname "$0")/../.." && pwd)
workspace=${R1_TEST_WORKSPACE:-$(dirname "$project")}
sdk="$workspace/vendor/beken/bk_avdk_smp/ap/components/bk_thirdparty/agora-iot-sdk"
cjson="$workspace/apps/netutils/cjson/cJSON"
out="$project/out/host/agora-session"
mkdir -p "$out"
gcc -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1 -no-pie \
  -I"$project/tools/agora/test_session_stubs" -I"$project/tools/xiaozhi/audio-test-stubs" \
  -I"$project/app/vision_badge/include" -I"$sdk/include/bk7258" -I"$cjson" \
  "$project/tools/agora/test_session.c" "$cjson/cJSON.c" -pthread -lm -o "$out/test_session"
if [ "$#" -gt 0 ]; then
  for scenario in "$@"; do
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" "$scenario"
  done
  exit 0
fi
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" mic-gain
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" power-drain
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" wake-admission
for scenario in control-transient control-busy control-disconnected control-auth-fail control-retry-limit; do
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" "$scenario"
done
for scenario in vision-handoff vision-multiple vision-burst vision-burst-multiple vision-http-burst vision-rx-alloc-fail vision-rx-limit vision-stop vision-timeout vision-camera-fail vision-http-fail vision-alloc-fail vision-quiesce-fail; do
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" "$scenario"
done
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" vision-long-poll-stop
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" early-join-greeting
for scenario in live-camera live-camera-stop live-camera-expire live-camera-fail live-camera-feedback-expire; do
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" "$scenario"
done
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" slow-send
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" send-cost
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" sched-failure
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" link-diagnostics
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" continuous-downlink
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" continuous-silence
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" continuous-noise
# A genuine codec round-trip catches periodic +/-4 residue that constant
# synthetic +/-3 blocks miss. No microphone, cloud, speaker or paid service.
ffmpeg -nostdin -hide_banner -loglevel error -f lavfi \
  -i anullsrc=r=16000:cl=mono -t 4 -c:a g722 -f g722 -y "$out/silence.g722"
ffmpeg -nostdin -hide_banner -loglevel error -f g722 -i "$out/silence.g722" \
  -f s16le -y "$out/silence.pcm"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" g722-silence-file "$out/silence.pcm"
# Keep the existing accelerated lifecycle tests, and separately verify the
# production350ms tail timeout with20ms frames and three consecutive turns.
gcc -D_GNU_SOURCE -DAGORA_RX_IDLE_MS=350u -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1 -no-pie \
  -I"$project/tools/agora/test_session_stubs" -I"$project/tools/xiaozhi/audio-test-stubs" \
  -I"$project/app/vision_badge/include" -I"$sdk/include/bk7258" -I"$cjson" \
  "$project/tools/agora/test_session.c" "$cjson/cJSON.c" -pthread -lm -o "$out/test_conversation"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_conversation" g722-conversation "$out/silence.pcm"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" join-failure
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" lease-join-failure
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_session" init-failure
gcc -D_GNU_SOURCE -DR1_WAKE_INTEGRATION=1 -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1 -no-pie \
  -I"$project/tools/agora/test_session_stubs" -I"$project/tools/xiaozhi/audio-test-stubs" \
  -I"$project/app/vision_badge/include" -I"$sdk/include/bk7258" -I"$cjson" \
  "$project/tools/agora/test_session.c" "$cjson/cJSON.c" -pthread -lm -o "$out/test_wake_integration"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_wake_integration" wake-scene-integration
