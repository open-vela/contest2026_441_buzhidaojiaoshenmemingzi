/* SPDX-License-Identifier: Apache-2.0 */
#ifndef R1_WAKE_MODEL_H
#define R1_WAKE_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct r1_wake_model_s;
struct r1_wake_model_diag_s
{
  unsigned int scores_milli[3];
  unsigned int input_low, input_high;
  bool valid;
};
int r1_wake_model_open(const unsigned char *data, size_t bytes, bool require_release,
                       struct r1_wake_model_s **out);
void r1_wake_model_close(struct r1_wake_model_s *model);
void r1_wake_model_gap(struct r1_wake_model_s *model);
/* Exactly one unamplified 20 ms / 16 kHz mono PCM block. 1 = trigger,
 * 0 = warming/listening; negative = failure. No cloud/device operation. */
int r1_wake_model_feed(struct r1_wake_model_s *model, const int16_t pcm[320], uint64_t end_ms,
                       float *score, bool *inferred);
size_t r1_wake_model_arena_used(const struct r1_wake_model_s *model);
int r1_wake_model_diagnostics(const struct r1_wake_model_s *model,
                            struct r1_wake_model_diag_s *diagnostic);
/* Fixed public synthetic feature input; checks target kernels without MIC,
 * Audio, network, user recordings or acceptance of the live wake feature. */
int r1_wake_model_selftest(int output_q[3]);
int r1_wake_model_frontend_selftest(unsigned int *changed, unsigned int *max_delta);
int r1_wake_model_set_threshold(struct r1_wake_model_s *model, unsigned int percent);
#ifdef __cplusplus
}
#endif
#endif
