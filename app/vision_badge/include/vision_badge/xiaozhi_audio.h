/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VISION_BADGE_XIAOZHI_AUDIO_H
#define VISION_BADGE_XIAOZHI_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XIAOZHI_PCM_RATE       16000
#define XIAOZHI_FRAME_SAMPLES  960
#define XIAOZHI_OPUS_MAX       1275
#define XIAOZHI_PLAYOUT_FRAMES 32
#define XIAOZHI_PREBUFFER      3

struct xiaozhi_codec_s;
struct xiaozhi_playout_s;

/* Encoder and decoder have separate single owners. Close/reset only after
 * both workers have joined. All output PCM is 16 kHz, including 24 kHz TTS.
 */
int xiaozhi_codec_open(struct xiaozhi_codec_s **codec);
void xiaozhi_codec_close(struct xiaozhi_codec_s *codec);
int xiaozhi_codec_encode(struct xiaozhi_codec_s *codec, const int16_t *pcm,
                         size_t samples, uint8_t *packet, size_t capacity);
int xiaozhi_codec_decode(struct xiaozhi_codec_s *codec, const uint8_t *packet,
                         size_t bytes, int16_t *pcm, size_t capacity);
int xiaozhi_codec_reset(struct xiaozhi_codec_s *codec);
int xiaozhi_codec_selftest(char *summary, size_t capacity);

struct xiaozhi_playout_stats_s
{
  unsigned int queued;
  uint32_t accepted;
  uint32_t consumed;
  uint32_t underruns;
  bool finished;
};

/* Bounded decoded-PCM queue: network/decode producer, playback consumer.
 * epoch rejects late work after abort; a TTS stop marks end-of-input only.
 * Pop returns samples, -EAGAIN while buffering, or 0 after all PCM drained.
 * Consumers must poll/wait outside the queue lock. Full queues return EAGAIN:
 * apply bounded backpressure, never silently drop a speech frame.
 */
int xiaozhi_playout_open(struct xiaozhi_playout_s **queue);
void xiaozhi_playout_close(struct xiaozhi_playout_s *queue);
uint32_t xiaozhi_playout_begin(struct xiaozhi_playout_s *queue);
int xiaozhi_playout_push(struct xiaozhi_playout_s *queue, uint32_t epoch,
                         const int16_t *pcm, size_t samples);
int xiaozhi_playout_finish(struct xiaozhi_playout_s *queue, uint32_t epoch);
int xiaozhi_playout_pop(struct xiaozhi_playout_s *queue, uint32_t epoch,
                        int16_t *pcm, size_t capacity);
int xiaozhi_playout_stats(struct xiaozhi_playout_s *queue, uint32_t epoch,
                          struct xiaozhi_playout_stats_s *stats);

#endif
