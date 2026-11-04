/* SPDX-License-Identifier: Apache-2.0 */
#ifndef R1_WAKE_BACKEND_H
#define R1_WAKE_BACKEND_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef CONFIG_CONTEST2026_441_WAKE_ASR
struct wake_backend_s;
int wake_backend_open(bool release, struct wake_backend_s **out);
void wake_backend_close(struct wake_backend_s *backend);
void wake_backend_gap(struct wake_backend_s *backend);
/* UDP uses the actual capture-callback monotonic timestamp (equal for
 * batched frames); HTTP/TCP use the continuous 20-ms waveform grid. */
int wake_backend_feed(struct wake_backend_s *backend, const int16_t pcm[320],
                      uint64_t end_ms, float *score, bool *inferred);
#else
#include <vision_badge/wake_model.h>
extern const unsigned char r1_wake_model_data[];
extern const size_t r1_wake_model_bytes;
#define wake_backend_s r1_wake_model_s
#define wake_backend_close r1_wake_model_close
#define wake_backend_gap r1_wake_model_gap
#define wake_backend_feed r1_wake_model_feed
static inline int wake_backend_open(bool release, struct wake_backend_s **out)
{
  return r1_wake_model_open(r1_wake_model_data,r1_wake_model_bytes,release,out);
}
#endif
#endif
