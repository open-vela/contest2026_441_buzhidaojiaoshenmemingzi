/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <stdbool.h>
static bool fail_app_alloc;
static void *test_app_malloc(size_t bytes)
{
  if (fail_app_alloc) return NULL;
  return malloc(bytes);
}
#define malloc test_app_malloc
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define mq_open fake_mq_open
#define mq_close fake_mq_close
#define mq_unlink fake_mq_unlink
#define mq_receive fake_mq_receive
#define sched_getparam fake_sched_getparam
#define sched_setparam fake_sched_setparam
#include "../../app/vision_badge/src/audio_service.c"
#undef malloc
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>

static bool capture;
static bool tone_test;
static int test_volume = 30;
static int test_input_peak = 1000;
static int capture_level = 1000;
static bool tag_capture_frames;
static unsigned int capture_frames;
static bool running;
static bool stopped;
static bool completion;
static bool fail_stop;
static bool no_frame;
static bool fail_reserve;
static bool failed_reserve_cleanup;
static unsigned int allocated;
static unsigned int freed;
static unsigned int starts;
static unsigned int failure_command;
static unsigned int releases;
static bool reserved;
static int current_priority = 99;
static bool fail_priority_get;
static int fail_priority_set;
static struct ap_buffer_s *fifo[16];
size_t bk7258_psram_free_size(void) { return 100000; }
size_t bk7258_psram_total_size(void) { return 655360; }
static unsigned int head;
static unsigned int tail;

int fake_sched_getparam(pid_t pid, struct sched_param *param)
{
  assert(pid == 0);
  if (fail_priority_get)
    {
      fail_priority_get = false;
      errno = ESRCH;
      return -1;
    }
  param->sched_priority = current_priority;
  return 0;
}

int fake_sched_setparam(pid_t pid, const struct sched_param *param)
{
  assert(pid == 0);
  if (fail_priority_set == param->sched_priority)
    {
      fail_priority_set = 0;
      errno = EPERM;
      return -1;
    }
  current_priority = param->sched_priority;
  return 0;
}

int fake_open(const char *path, int flags, ...)
{
  (void)flags;
  assert(!running && !reserved);
  capture = strstr(path, "pcm0c") != NULL;
  if (capture) capture_frames = 0;
  completion = false;
  stopped = false;
  head = tail = 0;
  return 10;
}

int fake_close(int fd)
{
  assert(fd == 10);
  assert(!running);
  return 0;
}

int fake_ioctl(int fd, unsigned long cmd, ...)
{
  va_list args;
  unsigned long arg;
  struct audio_buf_desc_s *desc;
  assert(fd == 10);
  va_start(args, cmd);
  arg = va_arg(args, unsigned long);
  va_end(args);
  desc = (void *)arg;
  if (failure_command == cmd)
    {
      failure_command = 0;
      errno = EIO;
      return -1;
    }
  switch (cmd)
    {
      case AUDIOIOC_RESERVE:
        if (fail_reserve)
          {
            fail_reserve = false;
            failed_reserve_cleanup = true;
            errno = ETIMEDOUT;
            return -1;
          }
        reserved = true;
        break;
      case AUDIOIOC_CONFIGURE:
        {
          struct audio_caps_desc_s *caps = (void *)arg;
          /* Mirror the current MIC/DAC format admission checks, rather
           * than silently accepting an empty configuration in the mock. */
          assert(caps->caps.ac_type ==
                 (capture ? AUDIO_TYPE_INPUT : AUDIO_TYPE_OUTPUT));
          assert(caps->caps.ac_subtype == AUDIO_FMT_PCM);
          assert(caps->caps.ac_channels == 1);
          assert(caps->caps.ac_controls.hw[0] == 16000);
          assert(caps->caps.ac_controls.b[2] == 16);
        }
        break;
      case AUDIOIOC_START:
        assert(tail > head); /* MIC must never start without receive APBs. */
        if (capture) assert(tail - head == g_audio.count);
        running = true;
        starts++;
        break;
      case AUDIOIOC_STOP:
        if (fail_stop)
          {
            fail_stop = false;
            errno = ETIMEDOUT;
            return -1;
          }
        running = false;
        stopped = true;
        head = tail;
        break;
      case AUDIOIOC_RELEASE:
        assert(!running);
        reserved = false;
        releases++;
        failed_reserve_cleanup = false;
        break;
      case AUDIOIOC_GETBUFFERINFO:
        ((struct ap_buffer_info_s *)arg)->nbuffers = 4;
        ((struct ap_buffer_info_s *)arg)->buffer_size = 640;
        break;
      case AUDIOIOC_ALLOCBUFFER:
        *desc->u.pbuffer = calloc(1, sizeof(struct ap_buffer_s));
        (*desc->u.pbuffer)->nmaxbytes = desc->numbytes;
        (*desc->u.pbuffer)->samp = calloc(1, desc->numbytes);
        allocated++;
        break;
      case AUDIOIOC_FREEBUFFER:
        assert(!running && !reserved);
        free(desc->u.buffer->samp);
        free(desc->u.buffer);
        freed++;
        break;
      case AUDIOIOC_ENQUEUEBUFFER:
        if (!capture && !tone_test)
          {
            if (desc->u.buffer->nbytes != 0)
              {
                assert(((int16_t *)desc->u.buffer->samp)[0] == -test_input_peak * test_volume / 100);
              }
            if (desc->u.buffer->nbytes >= 4)
              {
                assert(((int16_t *)desc->u.buffer->samp)[1] == test_input_peak * test_volume / 100);
              }
          }
        assert(tail - head < 16);
        fifo[tail++ % 16] = desc->u.buffer;
        break;
      default:
        break;
    }
  return 0;
}

mqd_t fake_mq_open(const char *name, int flags, ...)
{
  (void)name;
  (void)flags;
  return (mqd_t)11;
}

int fake_mq_close(mqd_t mq)
{
  (void)mq;
  return 0;
}

int fake_mq_unlink(const char *name)
{
  (void)name;
  return 0;
}

ssize_t fake_mq_receive(mqd_t mq, char *data, size_t size, unsigned int *pri)
{
  struct audio_msg_s *msg = (void *)data;
  struct ap_buffer_s *apb;
  unsigned int i;
  (void)mq;
  (void)pri;
  assert(size == sizeof(*msg));
  if (no_frame)
    {
      errno = EAGAIN;
      return -1;
    }
  if (head < tail)
    {
      apb = fifo[head++ % 16];
      if (capture)
        {
          int level = tag_capture_frames ? 1000 + (int)capture_frames : capture_level;
          apb->nbytes = apb->nmaxbytes;
          for (i = 0; i < apb->nbytes / 2; i++)
            {
              ((int16_t *)apb->samp)[i] = i % 2 == 0 ? -level : level;
            }
          capture_frames++;
        }
      else if ((apb->flags & AUDIO_APB_FINAL) != 0)
        {
          completion = true;
        }
      msg->msg_id = AUDIO_MSG_DEQUEUE;
      msg->u.ptr = apb;
      return sizeof(*msg);
    }
  if (completion)
    {
      completion = false;
      msg->msg_id = AUDIO_MSG_COMPLETE;
      return sizeof(*msg);
    }
  errno = EAGAIN;
  return -1;
}

void *bk7258_psram_malloc(size_t bytes)
{
  return malloc(bytes);
}

void bk7258_psram_free(void *ptr)
{
  free(ptr);
}

struct stream_test_s
{
  unsigned int chunks;
  unsigned int polls;
  bool done;
};

static bool stream_cancel(void *arg)
{
  struct stream_test_s *test = arg;
  return test->done;
}

static int stream_sink(void *arg, const int16_t *pcm, size_t samples)
{
  struct stream_test_s *test = arg;
  assert(samples == 320 && pcm[0] == -1000);
  test->done = ++test->chunks == 12;
  return 0;
}

static int tagged_stream_sink(void *arg, const int16_t *pcm, size_t samples)
{
  struct stream_test_s *test = arg;
  assert(samples == 320);
  for (size_t i = 0; i < samples; i++)
    assert(pcm[i] == (i % 2 ? 1 : -1) * (int)(1000 + test->chunks));
  test->done = ++test->chunks == 12;
  return 0;
}

static int stream_source(void *arg, int16_t *pcm, size_t samples)
{
  struct stream_test_s *test = arg;
  size_t i;
  if (++test->polls < 3)
    {
      return -EAGAIN;
    }
  if (test->chunks == 16)
    {
      return 0;
    }
  test->chunks++;
  for (i = 0; i < samples; i++)
    {
      pcm[i] = i % 2 == 0 ? -1000 : 1000;
    }
  return samples;
}

static bool cancel_no_frame(void *arg)
{
  struct stream_test_s *test = arg;
  return ++test->polls > 5;
}

static int volume_source(void *arg, int16_t *pcm, size_t samples)
{
  unsigned int *step=arg;
  char text[128];
  if (*step==2) return 0;
  assert(vision_badge_audio_diagnostic(*step ? "volume 8" : "volume 3", text, sizeof(text))==0);
  test_volume=*step ? 27 : 3;
  for(size_t i=0;i<samples;i++)pcm[i]=i%2 ? 1000 : -1000;
  (*step)++;return (int)samples;
}

static bool cancel_immediately(void *arg)
{
  (void)arg;
  return true;
}

static int empty_source(void *arg, int16_t *pcm, size_t samples)
{
  (void)arg; (void)pcm; (void)samples;
  return 0;
}

int main(void)
{
#ifdef CONFIG_CONTEST2026_441_POWER
  assert(audio_service_volume_level()==5 && audio_output_volume(30)==8);
  /* Exercise the legacy caller-gain cases below as well as the product
   * default. Product reset/playback is verified again at the end. */
  g_output_level=0;
#else
  assert(audio_service_volume_level()==0 && audio_output_volume(30)==30);
#endif
  int16_t pcm[3200];
  size_t bytes;
  char summary[256];
  unsigned int before;
  struct stream_test_s stream = {0};
  assert(audio_service_record(NULL, 1, &bytes) == -EINVAL);
  assert(audio_service_play(pcm, 3, 30) == -EINVAL);
  assert(audio_service_play(pcm, sizeof(pcm), 101) == -EINVAL);
  assert(audio_service_loopback(0, 30, summary, sizeof(summary)) == -EINVAL);
  before = allocated;
  fail_priority_get = true;
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == -ESRCH);
  fail_priority_set = 115;
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == -EPERM);
  assert(allocated == before && current_priority == 99 && !reserved);
  failure_command = AUDIOIOC_ENQUEUEBUFFER;
  before = starts;
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == -EIO);
  assert(starts == before && allocated == freed && current_priority == 99);
  failure_command = AUDIOIOC_START;
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == -EIO);
  assert(starts == before && allocated == freed && current_priority == 99);
  current_priority = 125;
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == 0);
  assert(current_priority == 125); /* Do not lower an already higher caller. */
  current_priority = 99;
  fail_priority_set = 99;
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == -EPERM);
  assert(allocated == freed && !reserved && !running && current_priority == 115);
  current_priority = 99;
  fail_reserve = true;
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == -ETIMEDOUT);
  assert(!failed_reserve_cleanup);
  assert(current_priority == 99);
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == 0);
  assert(bytes == sizeof(pcm));
  assert(pcm[0] == -1000 && pcm[3199] == 1000);
  assert(audio_service_play(pcm, sizeof(pcm), 30) == 0);
  assert(audio_service_play(pcm, 2, 30) == 0);
  assert(audio_service_loopback(1, 30, summary, sizeof(summary)) == 0);
  assert(strstr(summary, "bytes=32000") != NULL);
  test_volume = 55;
  assert(audio_service_loopback(5, 55, summary, sizeof(summary)) == 0);
  test_volume = 30;
  assert(strstr(summary, "bytes=160000") && strstr(summary, "mean="));
  fail_app_alloc = true;
  assert(audio_service_loopback(5, 55, summary, sizeof(summary)) == -ENOMEM);
  assert(strstr(summary, "step=allocate") && strstr(summary, "bytes=160000"));
  fail_app_alloc = false;
  assert(vision_badge_audio_diagnostic("mem", summary, sizeof(summary)) == 0);
  assert(strstr(summary, "ret=-12") && strstr(summary, "sdk_free=100000"));
  assert(vision_badge_audio_diagnostic("mic", summary, sizeof(summary)) == 0);
  assert(strstr(summary, "no speech validation"));
  before = starts;
  assert(vision_badge_audio_diagnostic("tone 0", summary, sizeof(summary)) == -EINVAL);
  assert(vision_badge_audio_diagnostic("tone 56", summary, sizeof(summary)) == -EINVAL);
  assert(vision_badge_audio_diagnostic("tone 30 extra", summary, sizeof(summary)) == -EINVAL);
  assert(starts == before);
  tone_test = true;
  assert(vision_badge_audio_diagnostic("tone 30", summary, sizeof(summary)) == 0);
  tone_test = false;
  assert(strstr(summary, "samples=16000") && starts == before + 1);
  unsigned int position = 0;
  assert(audio_tone_source(&position, pcm, 32) == 32 && pcm[0] == 0);
  position = 160;
  assert(audio_tone_source(&position, pcm, 16) == 16 && pcm[4] == 3000 && pcm[12] == -3000);
  position = 15999;
  assert(audio_tone_source(&position, pcm, 320) == 1 && position == 16000);
  assert(audio_tone_source(&position, pcm, 320) == 0);
  assert(allocated == freed);
  no_frame = true;
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == -ETIMEDOUT);
  no_frame = false;
  assert(allocated == freed);
  before = freed;
  fail_stop = true;
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == -ETIMEDOUT);
  assert(freed == before); /* failed STOP cannot free driver-owned buffers */
  assert(current_priority == 99);
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == 0);
  assert(allocated == freed && !running);
  assert(audio_service_capture_stream(stream_sink, stream_cancel, &stream) == 0);
  assert(stream.chunks == 12 && allocated == freed && !running);
  tag_capture_frames = true;
  for (unsigned int turn = 0; turn < 3; turn++)
    {
      memset(&stream, 0, sizeof(stream));
      assert(audio_service_capture_stream(tagged_stream_sink, stream_cancel, &stream) == 0);
      assert(stream.chunks == 12 && capture_frames == 12);
      assert(allocated == freed && !running && !reserved);
    }
  tag_capture_frames = false;
  puts("AUDIO_STREAM_ONSET_PASS three restarts retain the first frame in order; device mocked");
  tag_capture_frames = true;
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == 0 && bytes == sizeof(pcm));
  for (size_t i = 0; i < sizeof(pcm) / sizeof(pcm[0]); i++)
    assert(pcm[i] == (i % 2 ? 1 : -1) * (int)(1000 + i / 320));
  assert(capture_frames == sizeof(pcm) / 640 && allocated == freed);
  tag_capture_frames = false;
  puts("AUDIO_RECORD_ONSET_PASS diagnostic keeps the same first-frame contract; device mocked");
  memset(&stream, 0, sizeof(stream));
  assert(audio_service_play_stream(stream_source, stream_cancel, &stream, 30) == 0);
  assert(stream.chunks == 16 && allocated == freed && !running);
  memset(&stream, 0, sizeof(stream));
  no_frame = true;
  assert(audio_service_capture_stream(stream_sink, cancel_no_frame, &stream) ==
         -ECANCELED);
  no_frame = false;
  assert(allocated == freed && !running);
  const unsigned int setup_failures[] = {
    AUDIOIOC_CONFIGURE, AUDIOIOC_GETBUFFERINFO, AUDIOIOC_ALLOCBUFFER,
    AUDIOIOC_REGISTERMQ, AUDIOIOC_START
  };
  for (size_t i = 0; i < sizeof(setup_failures) / sizeof(setup_failures[0]); i++)
    {
      failure_command = setup_failures[i];
      assert(audio_service_record(pcm, sizeof(pcm), &bytes) == -EIO);
      assert(allocated == freed && !running && !reserved);
      assert(current_priority == 99);
      assert(audio_service_record(pcm, sizeof(pcm), &bytes) == 0);
    }
  before = starts;
  assert(audio_service_capture_stream(stream_sink, cancel_immediately, &stream) == -ECANCELED);
  assert(audio_service_play_stream(stream_source, cancel_immediately, &stream, 30) == -ECANCELED);
  assert(starts == before && allocated == freed && !reserved);
  assert(audio_service_play_stream(empty_source, stream_cancel, &(struct stream_test_s){0}, 30) == 0);
  assert(starts == before && allocated == freed && !reserved);
  memset(&stream, 0, sizeof(stream));
  before = freed;
  no_frame = true; fail_stop = true;
  assert(audio_service_capture_stream(stream_sink, cancel_no_frame, &stream) == -ETIMEDOUT);
  assert(freed == before && running && reserved);
  no_frame = false;
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == 0);
  assert(allocated == freed && !running && !reserved);
  before = freed;
  failure_command = AUDIOIOC_RELEASE;
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == -EIO);
  assert(freed == before && reserved);
  assert(current_priority == 99);
  assert(audio_service_record(pcm, sizeof(pcm), &bytes) == 0);
  assert(allocated == freed && !reserved && releases > 0);
  before = starts;
  assert(vision_badge_audio_diagnostic("cache status", summary, sizeof(summary)) == 0);
  assert(strstr(summary, "bytes=0") && starts == before);
  assert(vision_badge_audio_diagnostic("cache play 5 stream", summary, sizeof(summary)) == -ENODATA);
  const char *invalid_cache[] = {"cache capture 0", "cache capture 11", "cache capture 5 extra",
                               "cache play 101 stream", "cache play 5 other", "cache play 5 stream extra"};
  for (size_t i = 0; i < sizeof(invalid_cache) / sizeof(invalid_cache[0]); i++)
    assert(vision_badge_audio_diagnostic(invalid_cache[i], summary, sizeof(summary)) == -EINVAL);
  assert(starts == before);
  assert(vision_badge_audio_diagnostic("cache capture 5", summary, sizeof(summary)) == 0);
  assert(starts == before + 1 && !running && !reserved);
  assert(strstr(summary, "captured=160000") && strstr(summary, "dc=0"));
  int16_t *retained = g_audio_cache.pcm;
  test_volume = 5;
  assert(vision_badge_audio_diagnostic("cache play 5 buffer", summary, sizeof(summary)) == 0);
  assert(strstr(summary, "output_peak=50"));
  assert(vision_badge_audio_diagnostic("cache play 5 stream", summary, sizeof(summary)) == 0);
  assert(g_audio_cache.pcm == retained && retained[0] == -1000 && retained[1] == 1000);
  test_input_peak = 8000;
  assert(vision_badge_audio_diagnostic("cache play 5 normalize", summary, sizeof(summary)) == 0);
  assert(strstr(summary, "pcm_peak=8000") && strstr(summary, "output_peak=400"));
  assert(retained[0] == -1000 && retained[1] == 1000); /* no in-place gain */
  test_input_peak = 1000;
  fail_app_alloc = true;
  assert(vision_badge_audio_diagnostic("cache capture 5", summary, sizeof(summary)) == -ENOMEM);
  fail_app_alloc = false;
  assert(g_audio_cache.pcm == retained);
  fail_reserve = true;
  assert(vision_badge_audio_diagnostic("cache capture 1", summary, sizeof(summary)) == -ETIMEDOUT);
  assert(g_audio_cache.pcm == retained && !reserved && !running);
  capture_level = 100;
  assert(vision_badge_audio_diagnostic("cache capture 1", summary, sizeof(summary)) == 0);
  test_input_peak = 1600;
  assert(vision_badge_audio_diagnostic("cache play 5 normalize", summary, sizeof(summary)) == 0);
  assert(strstr(summary, "pcm_peak=1600")); /* amplification capped at16x */
  capture_level = 30000;
  assert(vision_badge_audio_diagnostic("cache capture 1", summary, sizeof(summary)) == 0);
  test_input_peak = 8000;
  assert(vision_badge_audio_diagnostic("cache play 5 normalize", summary, sizeof(summary)) == 0);
  assert(g_audio_cache.pcm[0] == -30000 && strstr(summary, "pcm_peak=8000"));
  capture_level = 0;
  assert(vision_badge_audio_diagnostic("cache capture 1", summary, sizeof(summary)) == 0);
  test_input_peak = 0;
  assert(vision_badge_audio_diagnostic("cache play 5 normalize", summary, sizeof(summary)) == 0);
  assert(strstr(summary, "pcm_peak=0")); /* never invent a waveform */
  before = starts;
  assert(vision_badge_audio_diagnostic("cache clear", summary, sizeof(summary)) == 0);
  assert(vision_badge_audio_diagnostic("cache clear", summary, sizeof(summary)) == 0);
  assert(g_audio_cache.pcm == NULL && g_audio_cache.bytes == 0 && starts == before);
  assert(vision_badge_audio_diagnostic("cache capture 10", summary, sizeof(summary)) == 0);
  assert(g_audio_cache.bytes == 320000);
  assert(vision_badge_audio_diagnostic("cache play 5 buffer", summary, sizeof(summary)) == 0);
  assert(strstr(summary, "bytes=320000") && g_audio_cache.bytes == 320000);
  assert(vision_badge_audio_diagnostic("cache clear", summary, sizeof(summary)) == 0);
  assert(allocated == freed && !reserved && !running);
  assert(current_priority == 99);
  const char *bad_levels[]={"volume 0","volume 11","volume -1","volume 1 extra","volume ","volume 01"};
  for(size_t i=0;i<sizeof(bad_levels)/sizeof(bad_levels[0]);i++)
    assert(vision_badge_audio_diagnostic(bad_levels[i],summary,sizeof(summary))==-EINVAL);
  for(unsigned int i=0;i<20;i++)assert(vision_badge_audio_diagnostic("volume down",summary,sizeof(summary))==0);
  assert(strstr(summary,"level=1/10") && audio_output_volume(30)==1);
  for(unsigned int i=0;i<20;i++)assert(vision_badge_audio_diagnostic("volume up",summary,sizeof(summary))==0);
  assert(strstr(summary,"level=10/10") && audio_output_volume(30)==55);
  unsigned int volume_step=0;
  test_input_peak=1000;
  assert(audio_service_play_stream(volume_source,audio_tone_cancel,&volume_step,30)==0);
  assert(volume_step==2 && audio_output_volume(30)==27);
  for(size_t i=0;i<sizeof(pcm)/sizeof(pcm[0]);i++)pcm[i]=i%2 ? 1000 : -1000;
  test_volume=27;assert(audio_service_play(pcm,sizeof(pcm),30)==0);
  assert(vision_badge_audio_diagnostic("volume reset",summary,sizeof(summary))==0);
#ifdef CONFIG_CONTEST2026_441_POWER
  assert(audio_service_volume_level()==5 && audio_output_volume(30)==8);
  test_volume=8;assert(audio_service_play(pcm,sizeof(pcm),30)==0);
  assert(allocated==freed && !running && !reserved);
  puts("PRODUCT_VOLUME_DEFAULT_PASS initial/reset level5; real PCM scaled8percent; caller fallback unchanged in legacy");
#else
  assert(audio_output_volume(30)==30);
#endif
  printf("audio service ownership/cache/live ten-level volume tests passed (%u starts)\n", starts);
  return 0;
}
