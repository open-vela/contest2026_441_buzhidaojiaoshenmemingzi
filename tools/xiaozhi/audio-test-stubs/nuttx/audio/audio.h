/* SPDX-License-Identifier: Apache-2.0 */
/* Minimal fields verified against pinned NuttX 76354c63 audio/audio.h.
 * Host fixtures test ownership/control flow, not binary layout or hardware.
 */
#include <stdbool.h>
#include <stdint.h>
#define AUDIO_TYPE_INPUT 1
#define AUDIO_TYPE_OUTPUT 2
#define AUDIO_FMT_PCM 6
#define AUDIO_APB_FINAL 1
#define AUDIO_MSG_DEQUEUE 1
#define AUDIO_MSG_COMPLETE 8
#define AUDIO_MSG_UNDERRUN 12
#define AUDIO_MSG_IOERR 13
enum
{
  AUDIOIOC_RESERVE = 100, AUDIOIOC_RELEASE, AUDIOIOC_REGISTERMQ,
  AUDIOIOC_UNREGISTERMQ, AUDIOIOC_CONFIGURE, AUDIOIOC_GETBUFFERINFO,
  AUDIOIOC_ALLOCBUFFER, AUDIOIOC_FREEBUFFER, AUDIOIOC_START,
  AUDIOIOC_STOP, AUDIOIOC_ENQUEUEBUFFER
};
struct audio_caps_s
{
  uint8_t ac_len, ac_type, ac_subtype, ac_channels;
  union { uint16_t hw; } ac_format;
  union { uint16_t hw[2]; uint8_t b[4]; } ac_controls;
};
struct audio_caps_desc_s { struct audio_caps_s caps; };
struct ap_buffer_info_s { uint16_t nbuffers, buffer_size; };
struct ap_buffer_s
{
  uint16_t nmaxbytes, nbytes, curbyte, flags;
  uint8_t *samp;
};
struct audio_buf_desc_s
{
  uint16_t numbytes;
  union { struct ap_buffer_s *buffer; struct ap_buffer_s **pbuffer; } u;
};
struct audio_msg_s
{
  uint16_t msg_id;
  union { void *ptr; uint32_t data; } u;
};
