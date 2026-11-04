/* SPDX-License-Identifier: Apache-2.0 */

#include <nuttx/config.h>

/* Product keys select one of ten output levels, initially level five.
 * Legacy profiles retain the caller's calibrated volume. No microphone gain.
 * The serialized RPC owner restores/saves the selection outside Audio I/O. */
#ifdef CONFIG_CONTEST2026_441_POWER
#  define AUDIO_DEFAULT_OUTPUT_LEVEL 5u
#else
#  define AUDIO_DEFAULT_OUTPUT_LEVEL 0u
#endif
static unsigned int g_output_level = AUDIO_DEFAULT_OUTPUT_LEVEL;
static const unsigned int g_output_percent[10] = {1, 2, 3, 5, 8, 12, 18, 27, 40, 55};
unsigned int audio_service_volume_level(void)
{
  return __atomic_load_n(&g_output_level, __ATOMIC_ACQUIRE);
}
static unsigned int audio_output_volume(unsigned int fallback)
{
  unsigned int level = __atomic_load_n(&g_output_level, __ATOMIC_ACQUIRE);
  return level ? g_output_percent[level - 1] : fallback;
}
#include <nuttx/audio/audio.h>
#include <nuttx/mutex.h>
#include <arch/chip/bk7258_psram.h>
#ifdef CONFIG_BK7258_AUD
#  include <arch/chip/bk7258_aud.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <vision_badge/config.h>
#include <vision_badge/services.h>
#include <vision_badge/rpc.h>

/* Use the public NuttX Audio ABI, not Armino tasks/direct SDK audio calls.
 * References: team 135 MIC/AUD and team 264 vs_audio.c (Apache-2.0).
 * Both devices are primed with queued buffers before START. Starting MIC
 * without receive buffers drops the first DMA completions.
 * Team135 voice_channel.c also explicitly removes startup flushing: once
 * fresh APBs are queued before START, its first frames are real input.
 * RELEASE also cleans a prepared (not running) DAC. Never free DMA-owned
 * buffers when STOP or RELEASE failed.
 */

#define AUDIO_RATE       16000u
#define AUDIO_BYTE_RATE  (AUDIO_RATE * 2u)
#define AUDIO_BUFFERS    8u
#define AUDIO_WAIT_MS    2000u
#define AUDIO_DIAGNOSTIC_MAX_SECONDS 10u

struct audio_session_s
{
  int fd;
  mqd_t mq;
  bool reserved;
  bool registered;
  bool started;
  bool queued[AUDIO_BUFFERS];
  struct ap_buffer_s *buffers[AUDIO_BUFFERS];
  unsigned int count;
};

/* Keep the context on failed teardown. The next request retries cleanup
 * rather than freeing DMA-owned memory or opening a competing session.
 */

static struct audio_session_s g_audio = { .fd = -1, .mq = (mqd_t)-1 };
static mutex_t g_audio_lock = NXMUTEX_INITIALIZER;
/* Only the serialized RPC worker updates these loopback observations. */
static size_t g_recorded;
static uint32_t g_peak;
static uint32_t g_mean_abs;
static int g_record_result;
static size_t g_buffer_request;
static int g_buffer_result;

/* Explicit diagnostic capture only. The AP RPC worker serializes access;
 * voice workers never reference this cache. At most one ten-second PCM is
 * retained until clear/replacement/reboot (two while replacing it).
 */
struct audio_cache_s
{
  int16_t *pcm;
  size_t bytes;
  uint32_t peak;
  uint32_t mean_abs;
  int32_t dc;
};

static struct audio_cache_s g_audio_cache;

static uint64_t audio_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int audio_ioctl(int command, unsigned long argument)
{
  if (ioctl(g_audio.fd, command, argument) < 0)
    {
      int error = errno;
      if (error != EAGAIN)
        syslog(LOG_ERR, "AUDIO ioctl=%d ret=%d\n", command, -error);
      return -error;
    }
  return 0;
}

static int audio_close(void)
{
  struct audio_buf_desc_s desc;
  unsigned int i;
  int ret;

  if (g_audio.fd < 0)
    {
      return 0;
    }

  if (g_audio.started)
    {
      ret = audio_ioctl(AUDIOIOC_STOP, 0);
      if (ret < 0)
        {
          return ret;
        }

      g_audio.started = false;
    }

  if (g_audio.reserved)
    {
      ret = audio_ioctl(AUDIOIOC_RELEASE, 0);
      if (ret < 0)
        {
          return ret;
        }

      g_audio.reserved = false;
    }

  if (g_audio.registered)
    {
      ret = audio_ioctl(AUDIOIOC_UNREGISTERMQ, (unsigned long)g_audio.mq);
      if (ret < 0)
        {
          return ret;
        }

      g_audio.registered = false;
    }

  for (i = 0; i < g_audio.count; i++)
    {
      if (g_audio.buffers[i] != NULL)
        {
          memset(&desc, 0, sizeof(desc));
          desc.u.buffer = g_audio.buffers[i];
          ret = audio_ioctl(AUDIOIOC_FREEBUFFER, (unsigned long)&desc);
          if (ret < 0)
            {
              return ret;
            }

          g_audio.buffers[i] = NULL;
        }
    }

  close(g_audio.fd);
  if (g_audio.mq != (mqd_t)-1)
    {
      mq_close(g_audio.mq);
      mq_unlink("/r1-audio");
    }

  memset(&g_audio, 0, sizeof(g_audio));
  g_audio.fd = -1;
  g_audio.mq = (mqd_t)-1;
  return 0;
}

static int audio_open(bool capture)
{
  struct audio_caps_desc_s caps;
  struct ap_buffer_info_s info;
  struct audio_buf_desc_s desc;
  struct mq_attr attr;
  unsigned int i;
  int ret = audio_close();

  if (ret < 0)
    {
      return ret;
    }

  g_audio.fd = open(capture ?
                    CONFIG_CONTEST2026_441_VISION_BADGE_AUDIO_DEVPATH :
                    "/dev/audio/pcm0p", O_RDWR);
  if (g_audio.fd < 0)
    {
      return -errno;
    }

  ret = audio_ioctl(AUDIOIOC_RESERVE, 0);
  if (ret < 0)
    {
      /* MIC can retain a power/frequency vote after a failed reservation.
       * Its RELEASE explicitly supports retrying that cleanup. EBUSY means
       * another owner: never release that owner's reservation.
       */

      g_audio.reserved = capture && ret != -EBUSY;
      return ret;
    }

  g_audio.reserved = true;
  memset(&caps, 0, sizeof(caps));
  caps.caps.ac_len = sizeof(caps.caps);
  caps.caps.ac_type = capture ? AUDIO_TYPE_INPUT : AUDIO_TYPE_OUTPUT;
  /* The DAC lower-half selects PCM by subtype, not the capability bitmap.
   * MIC accepts the same subtype; both playback paths share this setup. */
  caps.caps.ac_subtype = AUDIO_FMT_PCM;
  caps.caps.ac_channels = 1;
  caps.caps.ac_format.hw = AUDIO_FMT_PCM;
  caps.caps.ac_controls.hw[0] = AUDIO_RATE;
  caps.caps.ac_controls.b[2] = 16;
  ret = audio_ioctl(AUDIOIOC_CONFIGURE, (unsigned long)&caps);
  if (ret < 0)
    {
      return ret;
    }

  memset(&info, 0, sizeof(info));
  ret = audio_ioctl(AUDIOIOC_GETBUFFERINFO, (unsigned long)&info);
  if (ret < 0)
    {
      return ret;
    }

  if (info.nbuffers == 0 || info.nbuffers > AUDIO_BUFFERS ||
      info.buffer_size < 2 || info.buffer_size > 4096 ||
      (info.buffer_size & 1) != 0)
    {
      return -EPROTO;
    }

  g_audio.count = info.nbuffers;
  for (i = 0; i < g_audio.count; i++)
    {
      memset(&desc, 0, sizeof(desc));
      desc.numbytes = info.buffer_size;
      desc.u.pbuffer = &g_audio.buffers[i];
      ret = audio_ioctl(AUDIOIOC_ALLOCBUFFER, (unsigned long)&desc);
      if (ret < 0 || g_audio.buffers[i] == NULL)
        {
          return ret < 0 ? ret : -ENOMEM;
        }
    }

  memset(&attr, 0, sizeof(attr));
  attr.mq_maxmsg = g_audio.count + 4;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  g_audio.mq = mq_open("/r1-audio", O_CREAT | O_EXCL | O_RDWR |
                       O_NONBLOCK, 0600, &attr);
  if (g_audio.mq == (mqd_t)-1)
    {
      return -errno;
    }

  ret = audio_ioctl(AUDIOIOC_REGISTERMQ, (unsigned long)g_audio.mq);
  if (ret < 0)
    {
      return ret;
    }

  g_audio.registered = true;
  return 0;
}

static int audio_enqueue(unsigned int i)
{
  struct audio_buf_desc_s desc;
  uint64_t deadline = audio_now_ms() + 100;
  int ret;

  memset(&desc, 0, sizeof(desc));
  desc.u.buffer = g_audio.buffers[i];
  do
    {
      ret = audio_ioctl(AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&desc);
      if (ret != -EAGAIN)
        {
          break;
        }

      usleep(1000);
    }
  while (audio_now_ms() < deadline);

  if (ret >= 0)
    {
      g_audio.queued[i] = true;
    }

  return ret;
}

static int audio_start(void)
{
  int ret = audio_ioctl(AUDIOIOC_START, 0);
  if (ret >= 0)
    {
      g_audio.started = true;
    }
  return ret;
}

/* COMPLETE, not the final network packet, means playback has drained.
 * Monotonic polling avoids TLS clock synchronization changing deadlines.
 */

static int audio_receive_cancel(struct audio_msg_s *msg,
                                 audio_cancel_cb_t cancel, void *arg)
{
  uint64_t deadline = audio_now_ms() + AUDIO_WAIT_MS;
  ssize_t n;
  unsigned int i;

  do
    {
      if (cancel != NULL && cancel(arg))
        {
          return -ECANCELED;
        }

      n = mq_receive(g_audio.mq, (char *)msg, sizeof(*msg), NULL);
      if (n < 0)
        {
          if (errno != EAGAIN && errno != EINTR)
            {
              return -errno;
            }

          usleep(1000);
          continue;
        }

      if (n != (ssize_t)sizeof(*msg))
        {
          return -EPROTO;
        }

      if (msg->msg_id == AUDIO_MSG_COMPLETE)
        {
          return -ENODATA;
        }

      if (msg->msg_id == AUDIO_MSG_IOERR ||
          msg->msg_id == AUDIO_MSG_UNDERRUN)
        {
          return -EPIPE;
        }

      if (msg->msg_id != AUDIO_MSG_DEQUEUE)
        {
          continue;
        }

      for (i = 0; i < g_audio.count; i++)
        {
          if (msg->u.ptr == g_audio.buffers[i] && g_audio.queued[i])
            {
              g_audio.queued[i] = false;
              return (int)i;
            }
        }

      return -EPROTO;
    }
  while (audio_now_ms() < deadline);

  return -ETIMEDOUT;
}

static int audio_receive(struct audio_msg_s *msg)
{
  return audio_receive_cancel(msg, NULL, NULL);
}

int audio_service_probe(struct vision_badge_probe_s *probe)
{
  int fd;
  if (probe == NULL)
    {
      return -EINVAL;
    }

  fd = open(CONFIG_CONTEST2026_441_VISION_BADGE_AUDIO_DEVPATH,
            O_RDONLY | O_NONBLOCK);
  probe->available = fd >= 0;
  probe->error_code = fd < 0 ? errno : 0;
  if (fd >= 0)
    {
      close(fd);
    }

  return probe->available ? 0 : -probe->error_code;
}

int audio_service_record(void *buffer, size_t capacity, size_t *recorded)
{
  struct audio_msg_s msg;
  struct ap_buffer_s *apb;
  unsigned int i;
  size_t take;
  uint64_t first_ms = 0;
  uint64_t last_ms = 0;
  uint64_t now;
  uint32_t max_gap_ms = 0;
  unsigned int chunks = 0;
  struct sched_param saved_priority;
  struct sched_param capture_priority = { .sched_priority = 115 };
  bool priority_changed = false;
  int priority_ret = 0;
  int cleanup;
  int ret;

  if (buffer == NULL || capacity == 0 || (capacity & 1) != 0 ||
      capacity > AUDIO_DIAGNOSTIC_MAX_SECONDS * AUDIO_BYTE_RATE || recorded == NULL)
    {
      return -EINVAL;
    }

  *recorded = 0;
  ret = nxmutex_trylock(&g_audio_lock);
  if (ret < 0)
    {
      return -EBUSY;
    }

  /* The synchronous diagnostic runs on RPC priority99, below eye rendering
   * at100. Match the existing voice/wake capture priority115 while waiting
   * for MIC APBs, then restore the caller on every cleanup path. */
  if (sched_getparam(0, &saved_priority) < 0)
    {
      ret = -errno;
      nxmutex_unlock(&g_audio_lock);
      return ret;
    }

  if (saved_priority.sched_priority < capture_priority.sched_priority)
    {
      if (sched_setparam(0, &capture_priority) < 0)
        {
          ret = -errno;
          nxmutex_unlock(&g_audio_lock);
          return ret;
        }

      priority_changed = true;
    }

  ret = audio_open(true);
  for (i = 0; ret >= 0 && i < g_audio.count; i++)
    {
      g_audio.buffers[i]->flags = 0;
      ret = audio_enqueue(i);
    }

  if (ret >= 0)
    {
      ret = audio_start();
    }

  while (ret >= 0 && *recorded < capacity)
    {
      ret = audio_receive(&msg);
      if (ret < 0)
        {
          break;
        }

      i = (unsigned int)ret;
      apb = g_audio.buffers[i];
      if (apb->nbytes == 0 || apb->nbytes > apb->nmaxbytes ||
          (apb->nbytes & 1) != 0)
        {
          ret = -EPROTO;
          break;
        }

      now = audio_now_ms();
      if (chunks == 0)
        {
          first_ms = now;
          syslog(LOG_INFO, "AUDIO record ready; retaining PCM now\n");
        }
      else if (now - last_ms > max_gap_ms)
        {
          max_gap_ms = now - last_ms;
        }
      last_ms = now;
      chunks++;
      take = capacity - *recorded;
      if (take > apb->nbytes) take = apb->nbytes;
      memcpy((char *)buffer + *recorded, apb->samp, take);
      *recorded += take;

      apb->flags = 0;
      ret = *recorded == capacity ? 0 : audio_enqueue(i);
    }

  cleanup = audio_close();
  if (priority_changed && sched_setparam(0, &saved_priority) < 0)
    {
      priority_ret = -errno;
    }

  syslog(LOG_INFO, "AUDIO record bytes=%lu chunks=%u span_ms=%lu "
         "max_gap_ms=%lu ret=%d cleanup=%d priority_restore=%d\n",
         (unsigned long)*recorded, chunks,
         (unsigned long)(last_ms - first_ms),
         (unsigned long)max_gap_ms, ret, cleanup, priority_ret);
  nxmutex_unlock(&g_audio_lock);
  return cleanup < 0 ? cleanup : ret < 0 ? ret : priority_ret;
}

int audio_service_play(const int16_t *pcm, size_t bytes, unsigned int volume)
{
  struct audio_msg_s msg;
  struct ap_buffer_s *apb;
  size_t submitted = 0;
  size_t count;
  size_t sample;
  unsigned int next = 0;
  unsigned int i = 0;
  int cleanup;
  int ret;

  if (pcm == NULL || bytes == 0 || (bytes & 1) != 0 || volume > 100 ||
      bytes > AUDIO_DIAGNOSTIC_MAX_SECONDS * AUDIO_BYTE_RATE)
    {
      return -EINVAL;
    }

  ret = nxmutex_trylock(&g_audio_lock);
  if (ret < 0)
    {
      return -EBUSY;
    }

  ret = audio_open(false);
  while (ret >= 0)
    {
      if (submitted < bytes)
        {
          apb = g_audio.buffers[i];
          count = bytes - submitted;
          if (count > apb->nmaxbytes)
            {
              count = apb->nmaxbytes;
            }

          unsigned int selected_volume = audio_output_volume(volume);
          for (sample = 0; sample < count / 2; sample++)
            {
              ((int16_t *)apb->samp)[sample] =
                (int32_t)pcm[submitted / 2 + sample] * (int32_t)selected_volume / 100;
            }

          apb->nbytes = count;
          apb->curbyte = 0;
          apb->flags = submitted + count == bytes ? AUDIO_APB_FINAL : 0;
          ret = audio_enqueue(i);
          if (ret < 0)
            {
              break;
            }

          submitted += count;
          if (++next < g_audio.count && submitted < bytes)
            {
              i = next;
              continue;
            }
        }

      if (!g_audio.started)
        {
          ret = audio_start();
          if (ret < 0)
            {
              break;
            }
        }

      ret = audio_receive(&msg);
      if (ret == -ENODATA)
        {
          ret = submitted == bytes ? 0 : -EPIPE;
          break;
        }

      if (ret >= 0)
        {
          i = (unsigned int)ret;
        }
    }

  cleanup = audio_close();
  nxmutex_unlock(&g_audio_lock);
  return cleanup < 0 ? cleanup : ret < 0 ? ret : 0;
}

int audio_service_capture_stream(audio_capture_cb_t sink,
                                 audio_cancel_cb_t cancel, void *arg)
{
  struct audio_msg_s msg;
  struct ap_buffer_s *apb;
  unsigned int i;
  uint64_t started_ms = audio_now_ms();
  size_t delivered = 0;
  int cleanup;
  int ret;
  if (sink == NULL || cancel == NULL)
    {
      return -EINVAL;
    }
  if (cancel(arg)) return -ECANCELED;
  if (nxmutex_trylock(&g_audio_lock) < 0)
    {
      return -EBUSY;
    }
  ret = audio_open(true);
  for (i = 0; ret >= 0 && i < g_audio.count; i++)
    {
      g_audio.buffers[i]->flags = 0;
      ret = audio_enqueue(i);
    }
  if (ret >= 0)
    {
      ret = cancel(arg) ? -ECANCELED : audio_start();
    }
  while (ret >= 0 && !cancel(arg))
    {
      ret = audio_receive_cancel(&msg, cancel, arg);
      if (ret < 0)
        {
          break;
        }
      i = (unsigned int)ret;
      apb = g_audio.buffers[i];
      if (apb->nbytes == 0 || apb->nbytes > apb->nmaxbytes ||
          (apb->nbytes & 1) != 0)
        {
          ret = -EPROTO;
          break;
        }
      if (delivered == 0)
        {
          syslog(LOG_INFO, "AUDIO stream first_frame after_ms=%lu samples=%lu discarded=0\n",
                 (unsigned long)(audio_now_ms() - started_ms),
                 (unsigned long)(apb->nbytes / 2));
        }
      ret = sink(arg, (const int16_t *)apb->samp, apb->nbytes / 2);
      if (ret < 0)
        {
          break;
        }
      delivered += apb->nbytes / 2;
      apb->flags = 0;
      ret = audio_enqueue(i);
    }
  cleanup = audio_close();
  nxmutex_unlock(&g_audio_lock);
  syslog(LOG_INFO, "AUDIO stream closed delivered=%lu elapsed_ms=%lu ret=%d cleanup=%d\n",
         (unsigned long)delivered, (unsigned long)(audio_now_ms() - started_ms), ret, cleanup);
  /* A cancellation is expected when switching from MIC to DAC, but a
   * failed hardware teardown is not. Preserve retained-resource errors. */
  return cleanup < 0 ? cleanup : ret < 0 ? ret : 0;
}

int audio_service_play_stream(audio_source_cb_t source,
                              audio_cancel_cb_t cancel, void *arg,
                              unsigned int volume)
{
  struct audio_msg_s msg;
  struct ap_buffer_s *apb;
  uint64_t deadline;
  uint64_t entered=audio_now_ms();
  unsigned int i = 0;
  unsigned int initial = 0;
  unsigned int startup_stalls = 0, running_stalls = 0;
  uint64_t submitted_samples = 0, dac_started_ms = 0;
  uint64_t startup_wait_ms = 0, running_wait_ms = 0, wait_max_ms = 0;
  size_t sample;
  bool final = false;
  bool stream_started = false;
  int cleanup;
  int ret;
  if (source == NULL || cancel == NULL || volume > 100)
    {
      return -EINVAL;
    }
  if (cancel(arg)) return -ECANCELED;
  if (nxmutex_trylock(&g_audio_lock) < 0)
    {
      return -EBUSY;
    }
  ret = audio_open(false);
  while (ret >= 0 && !cancel(arg))
    {
      if (!final)
        {
          apb = g_audio.buffers[i];
          deadline = audio_now_ms() + (g_audio.started ? AUDIO_WAIT_MS : 10000u);
          bool waiting = false;
          uint64_t wait_started_ms = 0;
          do
            {
              if (cancel(arg))
                {
                  ret = -ECANCELED;
                  break;
                }
              ret = source(arg, (int16_t *)apb->samp, apb->nmaxbytes / 2);
              if (ret == -EAGAIN)
                {
                  if (!waiting)
                    {
                      waiting=true; wait_started_ms=audio_now_ms();
                      if (g_audio.started) running_stalls++; else startup_stalls++;
                    }
                  /* DAC remains open during rebuffering; no per-chunk reset.
                   * Driver diagnostics still record actual DMA starvation.
                   */
                  usleep(1000);
                }
            }
          while (ret == -EAGAIN && audio_now_ms() < deadline);
          if (waiting)
            {
              uint64_t waited=audio_now_ms()-wait_started_ms;
              if (g_audio.started) running_wait_ms+=waited; else startup_wait_ms+=waited;
              if (waited>wait_max_ms) wait_max_ms=waited;
            }
          if (ret == -EAGAIN)
            {
              ret = -ETIMEDOUT;
            }
          if (ret < 0 || (size_t)ret > apb->nmaxbytes / 2)
            {
              ret = ret < 0 ? ret : -EPROTO;
              break;
            }
          final = ret == 0;
          /* An empty stream has no PCM for DAC START. Release the prepared
           * device without enabling DMA/PA or reporting a false underrun. */
          if (final && initial == 0 && !g_audio.started) break;
          apb->nbytes = (unsigned int)ret * 2;
          apb->curbyte = 0;
          apb->flags = final ? AUDIO_APB_FINAL : 0;
          unsigned int selected_volume = audio_output_volume(volume);
          for (sample = 0; sample < apb->nbytes / 2; sample++)
            {
              ((int16_t *)apb->samp)[sample] =
                (int32_t)((int16_t *)apb->samp)[sample] * (int32_t)selected_volume / 100;
            }
          size_t enqueued_samples=apb->nbytes/2;
          ret = audio_enqueue(i);
          if (ret < 0)
            {
              break;
            }
          submitted_samples+=enqueued_samples;
          if (++initial < g_audio.count && !final)
            {
              i = initial;
              continue;
            }
        }
      if (!g_audio.started)
        {
          ret = audio_start();
          if (ret < 0)
            {
              break;
            }
          syslog(LOG_INFO,"AUDIO reply DAC START after_ms=%llu; not acoustic onset\n",
                 (unsigned long long)(audio_now_ms()-entered));
          dac_started_ms=audio_now_ms();
          stream_started=true;
        }
      ret = audio_receive_cancel(&msg, cancel, arg);
      if (ret == -ENODATA)
        {
          ret = final ? 0 : -EPIPE;
          break;
        }
      if (ret >= 0)
        {
          i = (unsigned int)ret;
        }
    }
  if (ret >= 0 && cancel(arg))
    {
      ret = -ECANCELED;
    }
  int underruns=-1;
#ifdef CONFIG_BK7258_AUD
  struct bk7258_aud_diag_s playback_diag;
  memset(&playback_diag,0,sizeof(playback_diag));
  if (stream_started && !bk7258_aud_get_diag(&playback_diag))
    underruns=(int)playback_diag.underrun_count;
#endif
  uint64_t dac_elapsed_ms=stream_started?audio_now_ms()-dac_started_ms:0;
  cleanup = audio_close();
  /* Completed-stream metadata, not recording and not an acoustic-rate
   * measurement. Compare source starvation/DMA underruns with TTS timing
   * before attributing perceived fast/slow speech to a sample-rate fault. */
  syslog(LOG_INFO,"AUDIO stream playback samples=%llu nominal_ms=%llu dac_ms=%llu startup_stalls=%u startup_wait_ms=%llu running_stalls=%u running_wait_ms=%llu wait_max_ms=%llu under=%d ret=%d cleanup=%d\n",
         (unsigned long long)submitted_samples,(unsigned long long)(submitted_samples*1000u/AUDIO_RATE),
         (unsigned long long)dac_elapsed_ms,startup_stalls,(unsigned long long)startup_wait_ms,
         running_stalls,(unsigned long long)running_wait_ms,(unsigned long long)wait_max_ms,underruns,ret,cleanup);
  nxmutex_unlock(&g_audio_lock);
  return cleanup < 0 ? cleanup : ret < 0 ? ret : 0;
}

int audio_service_loopback(unsigned int seconds, unsigned int volume,
                           char *summary, size_t capacity)
{
  int16_t *pcm;
  uint64_t absolute = 0;
  uint32_t peak = 0;
  size_t recorded = 0;
  size_t i;
  int ret;

  if (seconds < 1 || seconds > 5 || volume > 100 ||
      summary == NULL || capacity == 0)
    {
      return -EINVAL;
    }

  /* This application buffer is copied into driver-owned DMA buffers.  Use
   * the NuttX application heap (including its PSRAM regions), not the small
   * private SDK heap also used by Wi-Fi/RTC.  Do not change DMA allocation. */
  g_buffer_request = seconds * AUDIO_BYTE_RATE;
  g_recorded = g_peak = g_mean_abs = 0;
  g_record_result = -ENODATA;
  pcm = malloc(g_buffer_request);
  g_buffer_result = pcm == NULL ? -ENOMEM : 0;
  if (pcm == NULL)
    {
      snprintf(summary, capacity, "audio step=allocate ret=%d bytes=%lu sdk_free=%lu",
               -ENOMEM, (unsigned long)g_buffer_request,
               (unsigned long)bk7258_psram_free_size());
      return -ENOMEM;
    }

  printf("R1 audio: recording %u seconds, speak now\n", seconds);
  ret = audio_service_record(pcm, seconds * AUDIO_BYTE_RATE, &recorded);
  g_record_result = ret;
  g_recorded = recorded;
  if (ret >= 0 && recorded != 0)
    {
      for (i = 0; i < recorded / 2; i++)
        {
          uint32_t value = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
          absolute += value;
          if (value > peak)
            {
              peak = value;
            }
        }

      printf("R1 audio: recorded=%lu peak=%lu mean_abs=%lu, playing %u%%\n",
             (unsigned long)recorded, (unsigned long)peak,
             (unsigned long)(absolute / (recorded / 2)), volume);
      g_peak = peak;
      g_mean_abs = absolute / (recorded / 2);
      ret = audio_service_play(pcm, recorded, volume);
    }

  snprintf(summary, capacity,
           "audio ret=%d bytes=%lu peak=%lu mean=%lu rate=16000 bits=16 mono volume=%u",
           ret, (unsigned long)recorded, (unsigned long)peak,
           (unsigned long)g_mean_abs, audio_output_volume(volume));
  free(pcm);
  return ret;
}

static bool audio_tone_cancel(void *arg)
{
  (void)arg;
  return false;
}

struct audio_cache_reader_s
{
  size_t position;
  uint32_t numerator;
  uint32_t denominator;
};

static int audio_cache_source(void *arg, int16_t *pcm, size_t capacity)
{
  struct audio_cache_reader_s *reader = arg;
  size_t remaining = g_audio_cache.bytes / sizeof(int16_t) - reader->position;
  size_t count = remaining < capacity ? remaining : capacity;
  for (size_t i = 0; i < count; i++)
    {
      pcm[i] = (int64_t)g_audio_cache.pcm[reader->position + i] *
               reader->numerator / reader->denominator;
    }
  reader->position += count;
  return (int)count;
}

static void audio_cache_discard(struct audio_cache_s *cache)
{
  if (cache->pcm != NULL)
    {
      explicit_bzero(cache->pcm, cache->bytes);
      free(cache->pcm);
    }
  memset(cache, 0, sizeof(*cache));
}

static int audio_cache_diagnostic(const char *action, char *text, size_t capacity)
{
  unsigned int seconds, volume;
  char path[16];
  char extra;
  int ret;
  if (!strcmp(action, "cache clear"))
    {
      audio_cache_discard(&g_audio_cache);
      snprintf(text, capacity, "audio cache cleared; no recording or playback");
      return 0;
    }
  if (!strcmp(action, "cache status"))
    {
      snprintf(text, capacity,
               "audio cache bytes=%lu peak=%lu mean=%lu dc=%ld; waveform only",
               (unsigned long)g_audio_cache.bytes,
               (unsigned long)g_audio_cache.peak,
               (unsigned long)g_audio_cache.mean_abs, (long)g_audio_cache.dc);
      return 0;
    }
  if (sscanf(action, "cache capture %u %c", &seconds, &extra) == 1)
    {
      struct audio_cache_s candidate = {0};
      uint64_t absolute = 0;
      int64_t sum = 0;
      size_t recorded = 0;
      if (seconds < 1 || seconds > AUDIO_DIAGNOSTIC_MAX_SECONDS) return -EINVAL;
      candidate.bytes = seconds * AUDIO_BYTE_RATE;
      candidate.pcm = malloc(candidate.bytes);
      if (candidate.pcm == NULL) return -ENOMEM;
      syslog(LOG_INFO, "AUDIO diagnostic capture begin seconds=%u; no playback\n", seconds);
      ret = audio_service_record(candidate.pcm, candidate.bytes, &recorded);
      if (ret < 0 || recorded != candidate.bytes)
        {
          audio_cache_discard(&candidate);
          snprintf(text, capacity, "audio cache capture ret=%d retained_bytes=%lu",
                   ret < 0 ? ret : -EIO, (unsigned long)g_audio_cache.bytes);
          return ret < 0 ? ret : -EIO;
        }
      for (size_t i = 0; i < recorded / sizeof(int16_t); i++)
        {
          int32_t sample = candidate.pcm[i];
          uint32_t magnitude = sample < 0 ? -sample : sample;
          sum += sample;
          absolute += magnitude;
          if (magnitude > candidate.peak) candidate.peak = magnitude;
        }
      candidate.dc = sum / (int32_t)(recorded / sizeof(int16_t));
      candidate.mean_abs = absolute / (recorded / sizeof(int16_t));
      audio_cache_discard(&g_audio_cache);
      g_audio_cache = candidate;
      snprintf(text, capacity,
               "audio cache captured=%lu peak=%lu mean=%lu dc=%ld; no playback; not speech validation",
               (unsigned long)candidate.bytes, (unsigned long)candidate.peak,
               (unsigned long)candidate.mean_abs, (long)candidate.dc);
      return 0;
    }
  if (sscanf(action, "cache play %u %15s %c", &volume, path, &extra) == 2)
    {
      struct audio_cache_reader_s reader = { .numerator = 1, .denominator = 1 };
      if (volume > 100 || (strcmp(path, "buffer") && strcmp(path, "stream") &&
                          strcmp(path, "normalize"))) return -EINVAL;
      if (g_audio_cache.pcm == NULL) return -ENODATA;
      if (!strcmp(path, "normalize") && g_audio_cache.peak != 0)
        {
          /* Diagnostic only: limit amplification to16x and target peak8000.
           * Never alter retained raw PCM or the live microphone/RTC path.
           */
          reader.numerator = g_audio_cache.peak < 500 ? 16 : 8000;
          reader.denominator = g_audio_cache.peak < 500 ? 1 : g_audio_cache.peak;
        }
      if (!strcmp(path, "buffer"))
        ret = audio_service_play(g_audio_cache.pcm, g_audio_cache.bytes, volume);
      else
        ret = audio_service_play_stream(audio_cache_source, audio_tone_cancel, &reader, volume);
      snprintf(text, capacity,
               "audio cache replay ret=%d path=%s bytes=%lu raw_peak=%lu pcm_peak=%lu volume=%u output_peak=%lu; raw retained",
               ret, path, (unsigned long)g_audio_cache.bytes,
               (unsigned long)g_audio_cache.peak,
               (unsigned long)(g_audio_cache.peak * reader.numerator / reader.denominator),
               audio_output_volume(volume), (unsigned long)(g_audio_cache.peak * reader.numerator /
                                       reader.denominator * audio_output_volume(volume) / 100));
      return ret;
    }
  return -EINVAL;
}

static int audio_tone_source(void *arg, int16_t *pcm, size_t samples)
{
  static const int16_t sine[16] =
    {0, 1148, 2121, 2772, 3000, 2772, 2121, 1148,
     0, -1148, -2121, -2772, -3000, -2772, -2121, -1148};
  unsigned int *position = arg;
  size_t i;
  if (samples > AUDIO_RATE - *position) samples = AUDIO_RATE - *position;
  for (i = 0; i < samples; i++, (*position)++)
    {
      unsigned int ramp = *position < 160 ? *position :
                          AUDIO_RATE - *position < 160 ? AUDIO_RATE - *position : 160;
      pcm[i] = (int32_t)sine[*position % 16] * (int32_t)ramp / 160;
    }
  return samples;
}

int vision_badge_audio_diagnostic(const char *action, char *text, size_t capacity)
{
  int ret;
  if (action == NULL || text == NULL || capacity == 0) return -EINVAL;
  if (!strcmp(action, "volume") || !strncmp(action, "volume ", 7))
    {
      unsigned int level = __atomic_load_n(&g_output_level, __ATOMIC_ACQUIRE);
      if (!level) level = 2; /* Current R1 production volume is 2 percent. */
      if (!strcmp(action, "volume up")) { if (level < 10) level++; }
      else if (!strcmp(action, "volume down")) { if (level > 1) level--; }
      else if (!strcmp(action, "volume reset")) level = AUDIO_DEFAULT_OUTPUT_LEVEL;
      else if (strcmp(action, "volume"))
        {
          if (!strcmp(action, "volume 10")) level = 10;
          else if (strlen(action) == 8 && action[7] >= '1' && action[7] <= '9')
            level = (unsigned int)(action[7] - '0');
          else return -EINVAL;
        }
      if (strcmp(action, "volume"))
        __atomic_store_n(&g_output_level, level, __ATOMIC_RELEASE);
      snprintf(text, capacity, "volume level=%u/10 percent=%u selected=%d; output PCM only",
               level ? level : 2, level ? g_output_percent[level - 1] : 2,
               __atomic_load_n(&g_output_level, __ATOMIC_ACQUIRE) != 0);
      return 0;
    }
  if (!strncmp(action, "cache ", 6))
    return audio_cache_diagnostic(action, text, capacity);
  if (!strncmp(action, "tone ", 5))
    {
      unsigned int volume, position = 0;
      char extra;
      if (sscanf(action, "tone %u %c", &volume, &extra) != 1 ||
          volume < 1 || volume > 55) return -EINVAL;
      ret = audio_service_play_stream(audio_tone_source, audio_tone_cancel,
                                       &position, volume);
      snprintf(text, capacity, "tone ret=%d hz=1000 samples=%u volume=%u", ret, position, audio_output_volume(volume));
      return ret;
    }
  if (!strcmp(action, "mem"))
    {
      snprintf(text, capacity, "audio app_alloc=%lu ret=%d sdk_free=%lu sdk_total=%lu; SDK free is not app heap free",
               (unsigned long)g_buffer_request, g_buffer_result,
               (unsigned long)bk7258_psram_free_size(),
               (unsigned long)bk7258_psram_total_size());
      return 0;
    }
  if (!strcmp(action, "mic"))
    {
      struct vision_badge_probe_s probe;
      memset(&probe, 0, sizeof(probe));
      ret = audio_service_probe(&probe);
      snprintf(text, capacity, "mic available=%d probe=%d last_capture=%d bytes=%lu peak=%lu mean=%lu; no speech validation",
               probe.available, ret, g_record_result, (unsigned long)g_recorded,
               (unsigned long)g_peak, (unsigned long)g_mean_abs);
      return ret;
    }
  if (!strcmp(action, "dac"))
    {
#ifdef CONFIG_BK7258_AUD
      struct bk7258_aud_diag_s diag;
      memset(&diag, 0, sizeof(diag));
      ret = bk7258_aud_get_diag(&diag);
      snprintf(text, capacity, "dac ret=%d state=%lu err=%ld res=0x%lx bytes=%lu/%lu irq=%lu pa=%lu/%lu on=%lu under=%lu gain=%lu/%lu",
               ret, (unsigned long)diag.state, (long)diag.first_error,
               (unsigned long)diag.resources, (unsigned long)diag.submitted_bytes,
               (unsigned long)diag.played_bytes, (unsigned long)diag.dma_isr_count,
               (unsigned long)diag.pa_on_count, (unsigned long)diag.pa_off_count,
               (unsigned long)diag.pa_enabled, (unsigned long)diag.underrun_count,
               (unsigned long)diag.digital_gain, (unsigned long)diag.analog_gain);
      return ret;
#else
      snprintf(text, capacity, "dac not configured");
      return -ENOSYS;
#endif
    }
  return -EINVAL;
}
