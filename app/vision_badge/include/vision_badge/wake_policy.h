/* SPDX-License-Identifier: Apache-2.0 */
#ifndef R1_WAKE_POLICY_H
#define R1_WAKE_POLICY_H
#include <stdbool.h>
#include <stdint.h>
/* Aligned RAM model loading and render scheduling measured 95..118 ms on
 * R1. Restore team135's 300 ms evaluation cadence, retaining every 20 ms
 * frontend frame and the bounded 800 ms queue / stale-score checks. */
#define R1_WAKE_SCORE_PERIOD_HOPS 15u
#define R1_WAKE_SCORE_MAX_GAP_MS 700u
#ifdef CONFIG_CONTEST2026_441_WAKE_ASR
/* LAN ASR has measured 1169ms request jitter. Keep two seconds of PCM while
 * a request is in flight, with a separate 1500ms decision-age bound. This is
 * a jitter buffer, not permission to replay arbitrarily old wake results.
 * Acoustic KWS keeps its original smaller queue and freshness contract. */
#define R1_WAKE_SCORE_MAX_AGE_MS 1500u
#define R1_WAKE_QUEUE_FRAMES 100u
#else
#define R1_WAKE_SCORE_MAX_AGE_MS 800u
#define R1_WAKE_QUEUE_FRAMES 40u
#endif
#ifdef __cplusplus
extern "C" {
#endif
struct r1_wake_policy_s
{
  uint64_t last_ms, triggered_ms;
  unsigned int hits;
  unsigned int threshold_percent;
  bool armed, timestamp_valid;
};
void r1_wake_policy_init(struct r1_wake_policy_s *policy, bool require_release);
void r1_wake_policy_gap(struct r1_wake_policy_s *policy);
int r1_wake_policy_set_threshold(struct r1_wake_policy_s *policy, unsigned int percent);
int r1_wake_policy_score(struct r1_wake_policy_s *policy, const float scores[3], uint64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
