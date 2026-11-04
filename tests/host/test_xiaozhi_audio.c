/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <opus.h>
#include <vision_badge/xiaozhi_audio.h>

static struct xiaozhi_playout_s *thread_queue;
static uint32_t thread_epoch;

static void *produce(void *unused)
{
  int16_t pcm[XIAOZHI_FRAME_SAMPLES];
  unsigned int i;
  unsigned int j;
  int ret;
  (void)unused;
  for (i = 0; i < 1000; i++)
    {
      for (j = 0; j < XIAOZHI_FRAME_SAMPLES; j++)
        {
          pcm[j] = (int16_t)i;
        }

      do
        {
          ret = xiaozhi_playout_push(thread_queue, thread_epoch, pcm,
                                      XIAOZHI_FRAME_SAMPLES);
          if (ret == -EAGAIN)
            {
              sched_yield();
            }
        }
      while (ret == -EAGAIN);
      assert(ret == 0);
    }

  assert(xiaozhi_playout_finish(thread_queue, thread_epoch) == 0);
  return NULL;
}

static void test_queue(void)
{
  struct xiaozhi_playout_s *q;
  struct xiaozhi_playout_stats_s stats;
  int16_t pcm[XIAOZHI_FRAME_SAMPLES] = {42};
  int16_t decoded[XIAOZHI_FRAME_SAMPLES];
  uint32_t epoch;
  uint32_t old;
  unsigned int i;
  unsigned int j;
  pthread_t producer;
  int ret;

  assert(xiaozhi_playout_open(&q) == 0);
  epoch = xiaozhi_playout_begin(q);
  assert(xiaozhi_playout_pop(q, epoch, decoded, 960) == -EAGAIN);
  for (i = 0; i < XIAOZHI_PREBUFFER; i++)
    {
      assert(xiaozhi_playout_push(q, epoch, pcm, 960) == 0);
    }
  for (i = 0; i < XIAOZHI_PREBUFFER; i++)
    {
      assert(xiaozhi_playout_pop(q, epoch, decoded, 960) == 960);
      assert(decoded[0] == 42);
    }
  assert(xiaozhi_playout_pop(q, epoch, decoded, 960) == -EAGAIN);
  assert(xiaozhi_playout_pop(q, epoch, decoded, 960) == -EAGAIN);
  assert(xiaozhi_playout_stats(q, epoch, &stats) == 0);
  assert(stats.underruns == 1);
  assert(xiaozhi_playout_push(q, epoch, pcm, 960) == 0);
  assert(xiaozhi_playout_pop(q, epoch, decoded, 960) == -EAGAIN);
  assert(xiaozhi_playout_finish(q, epoch) == 0);
  assert(xiaozhi_playout_pop(q, epoch, decoded, 960) == 960);
  assert(xiaozhi_playout_pop(q, epoch, decoded, 960) == 0);
  assert(xiaozhi_playout_push(q, epoch, pcm, 960) == -EPIPE);
  old = epoch;
  epoch = xiaozhi_playout_begin(q);
  assert(epoch != old);
  assert(xiaozhi_playout_push(q, old, pcm, 960) == -ESTALE);
  assert(xiaozhi_playout_pop(q, old, decoded, 960) == -ESTALE);
  assert(xiaozhi_playout_finish(q, old) == -ESTALE);
  assert(xiaozhi_playout_push(q, epoch, pcm, 959) == -EINVAL);
  assert(xiaozhi_playout_pop(q, epoch, decoded, 959) == -EINVAL);
  for (i = 0; i < XIAOZHI_PLAYOUT_FRAMES; i++)
    {
      assert(xiaozhi_playout_push(q, epoch, pcm, 960) == 0);
    }
  assert(xiaozhi_playout_push(q, epoch, pcm, 960) == -EAGAIN);
  epoch = xiaozhi_playout_begin(q); /* abort drops old queued frames */
  assert(xiaozhi_playout_finish(q, epoch) == 0);
  assert(xiaozhi_playout_pop(q, epoch, decoded, 960) == 0);

  thread_queue = q;
  thread_epoch = xiaozhi_playout_begin(q);
  assert(pthread_create(&producer, NULL, produce, NULL) == 0);
  i = 0;
  for (;;)
    {
      ret = xiaozhi_playout_pop(q, thread_epoch, decoded, 960);
      if (ret == -EAGAIN)
        {
          sched_yield();
          continue;
        }
      if (ret == 0)
        {
          break;
        }
      assert(ret == 960);
      for (j = 0; j < 960; j++)
        {
          assert(decoded[j] == (int16_t)i);
        }
      i++;
    }
  assert(pthread_join(producer, NULL) == 0);
  assert(i == 1000);
  assert(xiaozhi_playout_stats(q, thread_epoch, &stats) == 0);
  assert(stats.accepted == 1000 && stats.consumed == 1000 && stats.finished);
  xiaozhi_playout_close(q);
}

static void test_codec(void)
{
  struct xiaozhi_codec_s *codec;
  OpusEncoder *encoder24;
  int16_t pcm[1440] = {0};
  int16_t decoded[960];
  uint8_t packet[XIAOZHI_OPUS_MAX];
  char summary[256];
  int error;
  int bytes;
  unsigned int i;

  assert(xiaozhi_codec_open(&codec) == 0);
  assert(xiaozhi_codec_encode(codec, pcm, 959, packet, sizeof(packet)) == -EINVAL);
  assert(xiaozhi_codec_decode(codec, packet, 0, decoded, 960) == -EINVAL);
  for (i = 0; i < 10; i++)
    {
      bytes = xiaozhi_codec_encode(codec, pcm, 960, packet, sizeof(packet));
      assert(bytes > 0 && bytes <= XIAOZHI_OPUS_MAX);
      assert(xiaozhi_codec_decode(codec, packet, bytes, decoded, 960) == 960);
    }
  encoder24 = opus_encoder_create(24000, 1, OPUS_APPLICATION_VOIP, &error);
  assert(encoder24 != NULL && error == OPUS_OK);
  bytes = opus_encode(encoder24, pcm, 1440, packet, sizeof(packet));
  assert(bytes > 0);
  assert(xiaozhi_codec_decode(codec, packet, bytes, decoded, 960) == 960);
  bytes = opus_encode(encoder24, pcm, 480, packet, sizeof(packet));
  assert(bytes > 0);
  assert(xiaozhi_codec_decode(codec, packet, bytes, decoded, 960) == -EPROTO);
  packet[0] = 3; /* code 3 with missing count byte */
  assert(xiaozhi_codec_decode(codec, packet, 1, decoded, 960) == -EPROTO);
  opus_encoder_destroy(encoder24);
  assert(xiaozhi_codec_reset(codec) == 0);
  xiaozhi_codec_close(codec);
  assert(xiaozhi_codec_selftest(summary, sizeof(summary)) == 0);
  assert(strstr(summary, "frames=10") != NULL);
}

int main(void)
{
  test_codec();
  test_queue();
  puts("Opus + bounded PCM queue: PASS (silent host test, no board timing)");
  return 0;
}
