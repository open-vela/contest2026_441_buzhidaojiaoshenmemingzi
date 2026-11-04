/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <math.h>
#include <string.h>
#include <vision_badge/wake_policy.h>

void r1_wake_policy_init(struct r1_wake_policy_s *p, bool require_release)
{
  memset(p, 0, sizeof(*p));
  p->armed = !require_release;
  p->threshold_percent = 85;
}
void r1_wake_policy_gap(struct r1_wake_policy_s *p)
{
  p->hits = 0; /* Do not release a triggered phrase merely because PCM stopped. */
}
int r1_wake_policy_set_threshold(struct r1_wake_policy_s *p, unsigned int percent)
{
  if (!p || percent < 50 || percent > 95) return -EINVAL;
  p->threshold_percent = percent;
  p->hits = 0; /* Changing sensitivity never releases a triggered phrase. */
  return 0;
}
int r1_wake_policy_score(struct r1_wake_policy_s *p, const float s[3], uint64_t ms)
{
  if (!p || !s) return -EINVAL;
  if (p->timestamp_valid && ms <= p->last_ms)
    { r1_wake_policy_gap(p); return -ESTALE; }
  if (p->timestamp_valid && ms - p->last_ms > R1_WAKE_SCORE_MAX_GAP_MS) r1_wake_policy_gap(p);
  p->last_ms = ms; p->timestamp_valid = true;
  float total = 0;
  for (unsigned int i = 0; i < 3; i++)
    {
      if (!isfinite(s[i]) || s[i] < 0 || s[i] > 1)
        { r1_wake_policy_gap(p); return -EPROTO; }
      total += s[i];
    }
  if (total < .95f || total > 1.05f)
    { r1_wake_policy_gap(p); return -EPROTO; }
  if (!p->armed && s[2] <= .20f && ms >= p->triggered_ms && ms - p->triggered_ms >= 1000)
    p->armed = true;
  if (!p->armed || s[2] < p->threshold_percent / 100.0f || s[2] <= s[0] || s[2] <= s[1])
    { p->hits = 0; return 0; }
  if (++p->hits < 2) return 0;
  p->hits = 0; p->armed = false; p->triggered_ms = ms;
  return 1;
}
