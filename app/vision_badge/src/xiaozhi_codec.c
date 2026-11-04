/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <opus.h>
#include <vision_badge/xiaozhi_audio.h>

struct xiaozhi_codec_s
{
  OpusEncoder *encoder;
  OpusDecoder *decoder;
};

void xiaozhi_codec_close(struct xiaozhi_codec_s *codec)
{
  if (codec != NULL)
    {
      opus_encoder_destroy(codec->encoder);
      opus_decoder_destroy(codec->decoder);
      free(codec);
    }
}

int xiaozhi_codec_open(struct xiaozhi_codec_s **result)
{
  struct xiaozhi_codec_s *codec;
  int error;

  if (result == NULL)
    {
      return -EINVAL;
    }

  *result = NULL;
  codec = calloc(1, sizeof(*codec));
  if (codec == NULL)
    {
      return -ENOMEM;
    }

  codec->encoder = opus_encoder_create(XIAOZHI_PCM_RATE, 1,
                                       OPUS_APPLICATION_VOIP, &error);
  if (codec->encoder == NULL || error != OPUS_OK)
    {
      goto failed;
    }

  codec->decoder = opus_decoder_create(XIAOZHI_PCM_RATE, 1, &error);
  if (codec->decoder == NULL || error != OPUS_OK)
    {
      goto failed;
    }

  error = opus_encoder_ctl(codec->encoder, OPUS_SET_BITRATE(24000));
  if (error == OPUS_OK)
    {
      error = opus_encoder_ctl(codec->encoder, OPUS_SET_COMPLEXITY(3));
    }

  if (error != OPUS_OK)
    {
      goto failed;
    }

  *result = codec;
  return 0;

failed:
  xiaozhi_codec_close(codec);
  return error == OPUS_ALLOC_FAIL ? -ENOMEM : -EIO;
}

int xiaozhi_codec_reset(struct xiaozhi_codec_s *codec)
{
  if (codec == NULL)
    {
      return -EINVAL;
    }

  if (opus_encoder_ctl(codec->encoder, OPUS_RESET_STATE) != OPUS_OK ||
      opus_decoder_ctl(codec->decoder, OPUS_RESET_STATE) != OPUS_OK)
    {
      return -EIO;
    }

  return 0;
}

int xiaozhi_codec_encode(struct xiaozhi_codec_s *codec, const int16_t *pcm,
                         size_t samples, uint8_t *packet, size_t capacity)
{
  int ret;
  if (codec == NULL || pcm == NULL || packet == NULL ||
      samples != XIAOZHI_FRAME_SAMPLES || capacity < XIAOZHI_OPUS_MAX)
    {
      return -EINVAL;
    }

  ret = opus_encode(codec->encoder, pcm, XIAOZHI_FRAME_SAMPLES,
                     packet, XIAOZHI_OPUS_MAX);
  return ret < 0 ? -EIO : ret;
}

int xiaozhi_codec_decode(struct xiaozhi_codec_s *codec, const uint8_t *packet,
                         size_t bytes, int16_t *pcm, size_t capacity)
{
  int ret;
  if (codec == NULL || pcm == NULL || packet == NULL || bytes == 0 ||
      bytes > XIAOZHI_OPUS_MAX || capacity < XIAOZHI_FRAME_SAMPLES)
    {
      return -EINVAL;
    }

  /* Xiaozhi v1 contract is mono / 60 ms. Opus decodes at the decoder's
   * output rate regardless of the encoder's 16/24 kHz input rate.
   */
  ret = opus_packet_get_nb_samples(packet, (opus_int32)bytes,
                                    XIAOZHI_PCM_RATE);
  if (ret != XIAOZHI_FRAME_SAMPLES ||
      opus_packet_get_nb_channels(packet) != 1)
    {
      return -EPROTO;
    }

  ret = opus_decode(codec->decoder, packet, (opus_int32)bytes, pcm,
                     XIAOZHI_FRAME_SAMPLES, 0);
  return ret == XIAOZHI_FRAME_SAMPLES ? ret : -EBADMSG;
}

int xiaozhi_codec_selftest(char *summary, size_t capacity)
{
  struct xiaozhi_codec_s *codec = NULL;
  int16_t *pcm;
  uint8_t *packet;
  unsigned int i;
  int bytes = 0;
  int ret;

  if (summary == NULL || capacity == 0)
    {
      return -EINVAL;
    }

  pcm = malloc(2 * XIAOZHI_FRAME_SAMPLES * sizeof(*pcm));
  packet = malloc(XIAOZHI_OPUS_MAX);
  ret = pcm == NULL || packet == NULL ? -ENOMEM : xiaozhi_codec_open(&codec);
  if (ret == 0)
    {
      for (i = 0; i < XIAOZHI_FRAME_SAMPLES; i++)
        {
          pcm[i] = (i % 40 < 20) ? 2000 : -2000;
        }

      for (i = 0; i < 10; i++)
        {
          bytes = xiaozhi_codec_encode(codec, pcm, XIAOZHI_FRAME_SAMPLES,
                                        packet, XIAOZHI_OPUS_MAX);
          ret = bytes < 0 ? bytes : xiaozhi_codec_decode(
            codec, packet, bytes, pcm + XIAOZHI_FRAME_SAMPLES,
            XIAOZHI_FRAME_SAMPLES);
          if (ret < 0)
            {
              break;
            }
        }

      if (ret >= 0)
        {
          ret = xiaozhi_codec_reset(codec);
        }
    }

  snprintf(summary, capacity, "codec ret=%d frames=%u last_packet=%d "
           "PCM=16000Hz/mono/60ms; no microphone or speaker",
           ret, ret == 0 ? 10u : 0u, bytes);
  xiaozhi_codec_close(codec);
  free(packet);
  free(pcm);
  return ret;
}
