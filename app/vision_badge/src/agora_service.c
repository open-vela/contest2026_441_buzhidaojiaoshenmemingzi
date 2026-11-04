/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <arch/chip/bk7258_psram.h>
#include <netutils/cJSON.h>
#include <agora_rtc_api.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <vision_badge/agora_service.h>
#include <vision_badge/agora_control.h>
#include <vision_badge/mimo_http.h>
#include <vision_badge/services.h>
#ifdef CONFIG_CONTEST2026_441_WAKE
#  include <vision_badge/wake_service.h>
static bool g_wake_busy, g_wake_fault;
#endif

/* Chip-owned, payload-free counters; these never issue an SDK/network call. */
extern void bk7258_agora_netreset(void);
extern void bk7258_agora_netstats(char *text, size_t capacity);

#ifndef AGORA_CONFIG
#  define AGORA_CONFIG "/mnt/sdnand/prov/agora.cfg"
#endif
#ifndef AGORA_JOIN_MS
#  define AGORA_JOIN_MS 15000u
#endif
#ifndef AGORA_PROBE_MS
#  define AGORA_PROBE_MS 3000u
#endif
#ifndef AGORA_RX_IDLE_MS
#  define AGORA_RX_IDLE_MS 350u
#endif
/* R1 observed quiet PCM peak3, but a real G722 round-trip of digital
 * silence also produces periodic peak4. A constant +/-3 test missed those
 * frames and allowed playback to restart.4/32768 is about -78dBFS.
 * This decoder-residue floor is not a microphone VAD threshold; the host
 * codec regression is not a substitute for pinned-BK-decoder validation. */
#define AGORA_RX_SILENCE_PEAK 4u
#ifndef AGORA_KEEPALIVE_MS
#  define AGORA_KEEPALIVE_MS 30000u
#endif
#ifndef AGORA_VISION_POLL_MS
#  define AGORA_VISION_POLL_MS 1000u
#endif
#ifndef AGORA_CONTROL_RETRY_MS
#  define AGORA_CONTROL_RETRY_MS 1000u
#endif
#define AGORA_CONTROL_MAX_FAILURES 3u
#define AGORA_VISION_BYTES 262144u
#define PCM_SAMPLES 320u /* G722 input: 16 kHz, mono, 20 ms. */
#define FRAME_MS 20u
#define CATCHUP_GAP_MS 10u
#define MAX_CATCHUP 2u
#define TX_PRIORITY 120
#define AUDIO_PRIORITY 125
#define LEASE_PRIORITY 99
#define TX_SAMPLES (PCM_SAMPLES * 16u)
#define RX_SAMPLES (PCM_SAMPLES * 32u)
/* The SDK receive callback cannot block while Audio is quiesced for a
 * <=10s observation. Retain that interval plus bounded delivery headroom
 * in the NuttX system heap, separate from the SDK's small private pool. */
#define RX_VISION_SAMPLES (16000u * 12u)
/* Team135's bounded TTS consumer uses an8192-byte startup watermark.
 * Keep source PCM intact; one20ms zero guard covers R1's10ms PA startup. */
#define RX_PREFILL_SAMPLES (8192u / sizeof(int16_t))

enum phase_e { IDLE, CONFIGURING, INITIALIZING, JOINING, ACTIVE, STOPPING, BLOCKED };
struct pcm_queue_s
{
  int16_t *data;
  size_t head, count, capacity;
};
struct agora_config_s
{
  char app_id[33], channel[64], token[513];
  uint32_t uid, remote_uid;
  unsigned int volume;
};

/* SRAM state and callbacks have process lifetime. Only the coordinator frees
 * queues/JSON. A failed fini retains everything that the SDK could reference.
 */
static struct
{
  mutex_t lock;
  enum phase_e phase;
  bool running, stop, silent, init_only, initialized, connection;
  bool sdk_init_failed;
  bool camera_paused, camera_live_owned, camera_live;
  bool rx_system;
  bool vision_enabled, vision_long_poll, observing, audio_quiesced;
  char vision_last[33];
  unsigned int vision_requests, vision_completed, vision_failed;
  uint64_t vision_flushed;
  int vision_error;
  unsigned int control_polls, control_failures, control_consecutive, control_recovered;
  uint64_t control_max_ms;
  bool joining, joined, receive, playing, audio_started, lease_started, blocked;
  connection_id_t conn;
  pthread_t audio, lease_thread;
  struct agora_config_s config;
  rtc_service_option_t service_options;
  rtc_channel_options_t channel_options;
  cJSON *local, *session;
  char lease[65];
  struct pcm_queue_s tx, rx;
  uint64_t last_rx, up, down;
  uint64_t response_rx_ms;
  unsigned int response_number;
  uint64_t captured, tx_started, tx_elapsed, last_send_started;
  uint64_t send_max_ms, send_gap_max_ms;
  size_t tx_high, capture_max;
  unsigned int send_calls, tx_overflows, rx_overflows;
  size_t rx_high, rx_largest;
  unsigned int remote_seen, expected_seen, rx_seen, rx_uid_drop, rx_state_drop;
  unsigned int mic_peak, mic_mean_max;
  unsigned int mic_gain, mic_tx_peak;
  uint64_t mic_clipped;
  unsigned int rx_peak, rx_last_peak, rx_zero_frames, rx_quiet_frames, rx_quiet_ignored;
  int error, sdk_error, buffer_error, audio_error, cleanup_error, control_error;
} g = { .lock = NXMUTEX_INITIALIZER, .mic_gain = 16 };
/* Explicit idle-only RAM opt-in until RTC + live camera is verified on R1. */
static bool g_camera_live_requested;

static uint64_t now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
}

/* Caller holds g.lock; no SDK/network/device call may run under this lock. */
static void fail_locked(int error)
{
  if (!g.error) g.error = error < 0 ? error : -EIO;
  g.stop = true;
}
static void fail(int error)
{
  nxmutex_lock(&g.lock); fail_locked(error); nxmutex_unlock(&g.lock);
}
static bool stopping(void *unused)
{
  bool stop;
  (void)unused;
  nxmutex_lock(&g.lock); stop = g.stop; nxmutex_unlock(&g.lock);
  return stop;
}
static void phase(enum phase_e value)
{
  nxmutex_lock(&g.lock); g.phase = value; nxmutex_unlock(&g.lock);
}
static void sdk_failure(int ret)
{
  if (!ret) return;
  nxmutex_lock(&g.lock); g.sdk_error = ret; fail_locked(-EIO); nxmutex_unlock(&g.lock);
}
static void control_failure(int ret)
{
  if (!ret) return;
  nxmutex_lock(&g.lock); g.control_error = ret; nxmutex_unlock(&g.lock);
}
static void cleanup_failure(int ret)
{
  if (!ret) return;
  nxmutex_lock(&g.lock);
  if (!g.cleanup_error) g.cleanup_error = ret;
  nxmutex_unlock(&g.lock);
}

static int step_result(const char *step, int ret)
{
  /* One bounded diagnostic per failing call, never SDK message/token/JSON. */
  if (ret) syslog(LOG_ERR, "agora: %s failed=%d\n", step, ret);
  return ret;
}

static void queue_put(struct pcm_queue_s *q, const void *pcm, size_t n)
{
  size_t tail = (q->head + q->count) % q->capacity;
  size_t first = q->capacity - tail;
  if (first > n) first = n;
  memcpy(q->data + tail, pcm, first * sizeof(int16_t));
  memcpy(q->data, (const char *)pcm + first * sizeof(int16_t), (n - first) * sizeof(int16_t));
  q->count += n;
}
static void queue_get(struct pcm_queue_s *q, int16_t *pcm, size_t n)
{
  size_t first = q->capacity - q->head;
  if (first > n) first = n;
  memcpy(pcm, q->data + q->head, first * sizeof(int16_t));
  memcpy(pcm + first, q->data, (n - first) * sizeof(int16_t));
  q->head = (q->head + n) % q->capacity;
  q->count -= n;
}

static void on_error(connection_id_t conn, int code, const char *message)
{
  (void)message; /* Vendor messages can contain credentials. */
  nxmutex_lock(&g.lock);
  if (g.running && code && (!g.connection || conn == g.conn))
    { g.sdk_error = code; fail_locked(-EIO); }
  nxmutex_unlock(&g.lock);
}
static void on_join(connection_id_t conn, uint32_t uid, int elapsed)
{
  (void)elapsed;
  nxmutex_lock(&g.lock);
  if (g.running && g.joining && g.connection && conn == g.conn && !g.stop)
    {
      if (uid != g.config.uid) fail_locked(-EPROTO);
      else { g.joined = true; g.receive = !g.silent; }
    }
  nxmutex_unlock(&g.lock);
}
static void on_disconnected(connection_id_t conn)
{
  nxmutex_lock(&g.lock);
  if (g.running && g.connection && conn == g.conn && !g.stop) fail_locked(-ENETDOWN);
  nxmutex_unlock(&g.lock);
  syslog(LOG_ERR, "AGORA RTC event=connection-lost\n");
}
static void on_reconnecting(connection_id_t conn)
{
  bool waiting = false;
  nxmutex_lock(&g.lock);
  if (g.running && g.connection && conn == g.conn)
    {
      waiting = g.joining && !g.joined;
      /* An initial connection retry is not the connection-lost event.
       * The coordinator already bounds this wait by AGORA_JOIN_MS.
       * Keep the existing fail-closed policy for an interrupted live
       * audio session; do not silently accumulate stale microphone data. */
      if (!waiting && !g.stop) fail_locked(-ENETDOWN);
    }
  nxmutex_unlock(&g.lock);
  syslog(LOG_INFO, "AGORA RTC event=reconnecting initial=%d\n", waiting);
}
static void on_remote_offline(connection_id_t conn, uint32_t uid, int reason)
{
  (void)reason;
  nxmutex_lock(&g.lock);
  if (g.running && g.connection && conn == g.conn && uid == g.config.remote_uid && !g.stop)
    fail_locked(-ENOTCONN);
  nxmutex_unlock(&g.lock);
}
static void on_remote_joined(connection_id_t conn, uint32_t uid, int elapsed)
{
  (void)elapsed;
  nxmutex_lock(&g.lock);
  if (g.running && g.connection && conn == g.conn && !g.stop)
    {
      g.remote_seen++;
      if (uid == g.config.remote_uid) g.expected_seen++;
    }
  nxmutex_unlock(&g.lock);
}
static void on_token_expiring(connection_id_t conn, const char *token)
{
  (void)token;
  nxmutex_lock(&g.lock);
  if (g.running && g.connection && conn == g.conn) fail_locked(-EACCES);
  nxmutex_unlock(&g.lock);
}
static void on_audio(connection_id_t conn, uint32_t uid, uint16_t timestamp,
                     const void *pcm, size_t bytes, const audio_frame_info_t *info)
{
  size_t samples = bytes / sizeof(int16_t);
  (void)timestamp;
  nxmutex_lock(&g.lock);
  if (!g.running || !g.connection || conn != g.conn) goto out;
  g.rx_seen++;
  /* The SDK may deliver expected-peer PCM before its join callback on a
   * different worker. Queue it for the existing JOINING connection; Audio
   * is not started until join succeeds. Stop/silent/foreign-UID rules remain. */
  if ((!g.receive && !g.joining) || g.stop || g.silent || (!g.joined && !g.joining))
    { g.rx_state_drop++; goto out; }
  if (uid != g.config.remote_uid) { g.rx_uid_drop++; goto out; }
  /* This API exposes no rate/channel fields. Only accept decoded PCM from
   * the configured G722 path, never reinterpret an encoded packet as PCM.
   */
  if (!pcm || !info || info->data_type != AUDIO_DATA_TYPE_PCM || !bytes ||
      bytes % sizeof(int16_t))
    { g.buffer_error = -EPROTO; fail_locked(-EPROTO); goto out; }
  if (samples > g.rx_largest) g.rx_largest = samples;
  if (samples > g.rx.capacity)
    { g.rx_overflows++; g.buffer_error = -ENOBUFS; fail_locked(-ENOBUFS); goto out; }
  unsigned int peak = 0;
  for (size_t i = 0; i < samples; i++)
    {
      int16_t sample;
      memcpy(&sample, (const char *)pcm + i * sizeof(sample), sizeof(sample));
      int value = sample;
      unsigned int absolute = value < 0 ? (unsigned int)-value : (unsigned int)value;
      if (absolute > peak) peak = absolute;
    }
  g.rx_last_peak = peak;
  if (peak > g.rx_peak) g.rx_peak = peak;
  if (!peak) g.rx_zero_frames++;
  if (peak <= AGORA_RX_SILENCE_PEAK)
    {
      g.rx_quiet_frames++;
      /* Decoded jitter/PLC silence is still a PCM callback. It must not
       * start the speaker or extend a completed half-duplex response.
       * Preserve short quiet gaps inside speech, then let COMPLETE drain
       * before capture resumes. G722 digital silence includes +/-4;
       * retain every sample when the frame peak exceeds that floor.
       * This is not a speech VAD or echo cancellation. */
      if (!g.playing || now_ms() - g.last_rx >= AGORA_RX_IDLE_MS)
        { g.rx_quiet_ignored++; goto out; }
    }
  if (samples > g.rx.capacity - g.rx.count)
    { g.rx_overflows++; g.buffer_error = -ENOBUFS; fail_locked(-ENOBUFS); goto out; }
  /* Numeric timestamps only; the SDK callback must not log or perform I/O.
   * This marks accepted non-quiet PCM, not user speech end or audible sound. */
  if (!g.playing) { g.response_rx_ms=now_ms(); g.response_number++; }
  queue_put(&g.rx, pcm, samples);
  if (g.rx.count > g.rx_high) g.rx_high = g.rx.count;
  g.down += samples;
  if (peak > AGORA_RX_SILENCE_PEAK) g.last_rx = now_ms();
  g.playing = true;
  g.tx.head = g.tx.count = 0; /* Pause uplink as downlink starts. */
out:
  nxmutex_unlock(&g.lock);
}
static const agora_rtc_event_handler_t g_events =
{
  .on_error = on_error, .on_join_channel_success = on_join,
  .on_rejoin_channel_success = on_join, .on_reconnecting = on_reconnecting,
  .on_connection_lost = on_disconnected, .on_user_offline = on_remote_offline,
  .on_user_joined = on_remote_joined,
  .on_token_privilege_will_expire = on_token_expiring, .on_audio_data = on_audio
};

static bool capture_cancel(void *unused)
{
  bool cancel;
  (void)unused;
  nxmutex_lock(&g.lock); cancel = g.stop || g.playing || g.observing; nxmutex_unlock(&g.lock);
  return cancel;
}
static int capture_pcm(void *unused, const int16_t *pcm, size_t samples)
{
  int ret = 0;
  size_t queued = 0;
  uint64_t elapsed = 0;
  (void)unused;
  nxmutex_lock(&g.lock);
  if (!g.stop && !g.playing && !g.observing)
    {
      uint64_t now = now_ms();
      if (!g.tx_started) g.tx_started = now;
      g.tx_elapsed = now - g.tx_started;
      if (samples > g.capture_max) g.capture_max = samples;
      g.captured += samples;
      if (!pcm || samples > g.tx.capacity - g.tx.count)
        {
          ret = !pcm ? -EINVAL : -ENOBUFS;
          if (ret == -ENOBUFS) g.tx_overflows++;
          queued = g.tx.count; elapsed = g.tx_elapsed;
          g.buffer_error = ret; fail_locked(ret);
        }
      else if (samples)
        {
          uint64_t sum = 0;
          for (size_t i = 0; i < samples; i++)
            {
              int value = pcm[i];
              unsigned int absolute = value < 0 ? (unsigned int)-value : (unsigned int)value;
              if (absolute > g.mic_peak) g.mic_peak = absolute;
              sum += absolute;
            }
          unsigned int mean = (unsigned int)(sum / samples);
          if (mean > g.mic_mean_max) g.mic_mean_max = mean;
          queue_put(&g.tx, pcm, samples);
          if (g.tx.count > g.tx_high) g.tx_high = g.tx.count;
        }
    }
  nxmutex_unlock(&g.lock);
  if (ret)
    syslog(LOG_ERR, "agora: capture queue failed=%d queued=%lu chunk=%lu elapsed=%llu ms\n",
           ret, (unsigned long)queued, (unsigned long)samples,
           (unsigned long long)elapsed);
  return ret;
}
/* Apply only to the coordinator's private 20ms send copy. Never modify
 * capture DMA buffers, diagnostic raw PCM, downlink audio or its volume.
 * Fixed gain is not noise suppression or an ASR/VAD guarantee. */
static unsigned int scale_tx_pcm(int16_t *pcm, size_t samples,
                                  unsigned int gain, uint64_t *clipped)
{
  unsigned int peak = 0;
  for (size_t i = 0; i < samples; i++)
    {
      int32_t value = (int32_t)pcm[i] * (int32_t)gain;
      if (value > INT16_MAX) { value = INT16_MAX; (*clipped)++; }
      else if (value < INT16_MIN) { value = INT16_MIN; (*clipped)++; }
      pcm[i] = (int16_t)value;
      unsigned int absolute = value < 0 ? (unsigned int)-value : (unsigned int)value;
      if (absolute > peak) peak = absolute;
    }
  return peak;
}

struct playback_source_s { size_t guard; bool buffering, first; uint64_t received; };
static int playback_pcm(void *arg, int16_t *pcm, size_t capacity)
{
  int ret;
  bool first=false;
  struct playback_source_s *state = arg;
  if (!pcm || !capacity) return -EINVAL;
  nxmutex_lock(&g.lock);
  if (g.stop) ret = -ECANCELED;
  else if (state && state->guard)
    {
      size_t count = state->guard < capacity ? state->guard : capacity;
      memset(pcm, 0, count * sizeof(*pcm));
      state->guard -= count;
      ret = (int)count;
    }
  else if (g.rx.count)
    {
      if (state && state->buffering && g.rx.count < RX_PREFILL_SAMPLES &&
          now_ms() - g.last_rx < AGORA_RX_IDLE_MS)
        ret = -EAGAIN;
      else
        {
          size_t n = g.rx.count < capacity ? g.rx.count : capacity;
          queue_get(&g.rx, pcm, n); ret = (int)n;
          if (state)
            { first=!state->first; state->first=true; state->buffering=false; }
        }
    }
  else ret = now_ms() - g.last_rx >= AGORA_RX_IDLE_MS ? 0 : -EAGAIN;
  nxmutex_unlock(&g.lock);
  if (first) syslog(LOG_INFO,"agora: first reply PCM source rx_wait=%llu ms; not acoustic onset\n",
                    (unsigned long long)(now_ms()-state->received));
  return ret;
}
static void *audio_worker(void *unused)
{
  (void)unused;
  unsigned int announced=0;
  while (!stopping(NULL))
    {
      bool play;
      int ret;
      nxmutex_lock(&g.lock);
      if (g.observing)
        {
          /* Only published after the preceding stream call returned through
           * its STOP/RELEASE cleanup. The camera never owns audio devices. */
          g.audio_quiesced = true;
          nxmutex_unlock(&g.lock);
          usleep(5000);
          continue;
        }
      g.audio_quiesced = false;
      play = g.playing;
      uint64_t received=g.response_rx_ms;
      unsigned int response=g.response_number;
      nxmutex_unlock(&g.lock);
      if (play)
        {
          if (response!=announced)
            {
              announced=response;
              syslog(LOG_INFO,"agora: reply RX turn=%u first_ms=%llu worker_wait=%llu ms\n",
                response,(unsigned long long)received,(unsigned long long)(now_ms()-received));
            }
          struct playback_source_s source = { .guard = PCM_SAMPLES, .buffering = true, .received=received };
          ret = audio_service_play_stream(playback_pcm, stopping, &source, g.config.volume);
          if (!ret) syslog(LOG_INFO, "agora: playback released; Audio cleanup complete\n");
          nxmutex_lock(&g.lock);
          /* Success includes COMPLETE + cleanup; capture stays paused until
           * that return, including the time when the sample queue is empty.
           */
          if (!ret && !g.rx.count) g.playing = false;
          nxmutex_unlock(&g.lock);
        }
      else ret = audio_service_capture_stream(capture_pcm, capture_cancel, NULL);
      nxmutex_lock(&g.lock);
      if (ret < 0 && !(ret == -ECANCELED && (g.stop || (!play && (g.playing || g.observing)))))
        { g.audio_error = ret; fail_locked(ret); }
      nxmutex_unlock(&g.lock);
    }
  return NULL;
}
static int create_worker(pthread_t *thread, void *(*entry)(void *),
                         bool detached, int priority)
{
  pthread_attr_t attr;
  int ret = pthread_attr_init(&attr);
  if (ret) return -ret;
  ret = pthread_attr_setstacksize(&attr, 32768);
  if (!ret && detached) ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  /* Audio has 20ms deadlines. Do not inherit the RPC worker's priority99
   * or compete on its 200ms RR quantum. SDK workers are explicitly95..115
   * in the HAL; keep voice below the driver/transport/supervisor workers. */
  struct sched_param param = { .sched_priority = priority };
  if (!ret) ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  if (!ret) ret = pthread_attr_setschedpolicy(&attr, SCHED_RR);
  if (!ret) ret = pthread_attr_setschedparam(&attr, &param);
#ifndef VISION_AGORA_HOST_TEST
  if (!ret)
    {
      cpu_set_t cpus;
      CPU_ZERO(&cpus); CPU_SET(0, &cpus);
      ret = pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
    }
#endif
  if (!ret) ret = pthread_create(thread, &attr, entry, NULL);
  pthread_attr_destroy(&attr);
  return -ret;
}

/* Reuse the single camera owner. Live mode copies post-request warm frames
 * while Audio continues; legacy mode waits for Audio cleanup. Cleanup joins
 * this worker before freeing local/lease or releasing either camera lease. */
static void observe_scene(struct agora_scene_request_s request)
{
  uint64_t started = now_ms(), deadline = started + request.remaining_ms;
  uint64_t capture_started = 0, captured = 0, uploaded = 0;
  struct vision_badge_image_s image = { .capacity = AGORA_VISION_BYTES,
                                       .format = VISION_BADGE_IMAGE_JPEG };
  int ret = 0;
  bool owns_pause = false, upload_attempted = false, fatal_camera = false;
  nxmutex_lock(&g.lock);
  if (!strcmp(g.vision_last, request.id))
    { nxmutex_unlock(&g.lock); return; } /* No POST/capture replay. */
  memcpy(g.vision_last, request.id, sizeof(g.vision_last));
  g.vision_requests++;
  nxmutex_unlock(&g.lock);
  if (g.camera_live) goto capture;
  for (;;)
    {
      nxmutex_lock(&g.lock);
      bool stop = g.stop, playing = g.playing;
      if (!stop && !playing)
        {
          g.observing = true; g.audio_quiesced = false;
          g.vision_flushed += g.tx.count;
          g.tx.head = g.tx.count = 0;
          owns_pause = true;
        }
      nxmutex_unlock(&g.lock);
      if (stop) { ret = -ECANCELED; goto done; }
      if (owns_pause) break;
      if (now_ms() >= deadline) { ret = -ETIMEDOUT; goto done; }
      usleep(5000);
    }
  for (;;)
    {
      nxmutex_lock(&g.lock);
      bool ready = g.audio_quiesced, stop = g.stop;
      nxmutex_unlock(&g.lock);
      if (stop) { ret = -ECANCELED; goto done; }
      if (ready) break;
      if (now_ms() >= deadline)
        { ret = -ETIMEDOUT; fail(ret); goto done; }
      usleep(5000);
    }
capture:
  if (now_ms() >= deadline) { ret = -ETIMEDOUT; goto done; }
  /* Same NuttX system heap as the existing camera snapshot path. The SDK
   * private PSRAM heap has <256KiB free on R1 even when idle; reserving a
   * full JPEG there prevents capture while RTC owns its buffers. */
  image.data = malloc(image.capacity);
  if (!image.data) { ret = -ENOMEM; goto done; }
  feedback_service_activity(VISION_BADGE_STAGE_CAPTURE);
  if (stopping(NULL)) { ret = -ECANCELED; goto done; }
  capture_started = now_ms();
  if (capture_started >= deadline) { ret = -ETIMEDOUT; goto done; }
  ret = g.camera_live ? camera_service_capture_live(&image,
           (unsigned int)(deadline - capture_started), stopping, NULL) :
         camera_service_capture_paused(&image);
  captured = now_ms();
  if (ret < 0)
    {
      /* Camera cleanup failure is ambiguous. Do not resume audio or
       * overlapping DMA after the camera has quarantined itself. */
      fatal_camera = !g.camera_live || (ret != -ETIMEDOUT && ret != -ECANCELED);
      goto done;
    }
  if (stopping(NULL)) { ret = -ECANCELED; goto done; }
  if (now_ms() >= deadline) { ret = -ETIMEDOUT; goto done; }
  feedback_service_activity(VISION_BADGE_STAGE_QUERY);
  if (stopping(NULL)) { ret = -ECANCELED; goto done; }
  uint64_t upload_started = now_ms();
  if (upload_started >= deadline) { ret = -ETIMEDOUT; goto done; }
  request.remaining_ms = (uint32_t)(deadline - upload_started);
  upload_attempted = true;
  ret = agora_control_scene_request(g.local, &request, &image);
  uploaded = now_ms();
done:
  if (image.data) explicit_bzero(image.data, image.capacity);
  free(image.data);
  if (ret && ret != -ECANCELED && !upload_attempted && !stopping(NULL) && now_ms() < deadline)
    {
      request.remaining_ms = (uint32_t)(deadline - now_ms());
      int reported = agora_control_scene_failed(g.local, &request);
      syslog(reported < 0 ? LOG_ERR : LOG_INFO,
             "agora: local scene failure=%d notification=%d; no upload replay\n", ret, reported);
    }
  if (fatal_camera) fail(ret);
  nxmutex_lock(&g.lock);
  if (owns_pause) g.observing = false;
  g.vision_error = ret;
  if (ret) g.vision_failed++; else g.vision_completed++;
  nxmutex_unlock(&g.lock);
  feedback_service_activity(ret == -ECANCELED ? VISION_BADGE_STAGE_IDLE :
                            ret < 0 ? VISION_BADGE_STAGE_ERROR :
                                      VISION_BADGE_STAGE_DONE);
  syslog(ret < 0 ? LOG_ERR : LOG_INFO,
         "agora: scene handoff ret=%d elapsed=%lu ms pre=%ld shot=%ld upload_total=%ld finish=%lu ms\n", ret,
         (unsigned long)(now_ms() - started),
         capture_started ? (long)(capture_started - started) : -1,
         captured ? (long)(captured - capture_started) : -1,
         uploaded ? (long)(uploaded - captured) : -1,
         (unsigned long)(now_ms() - (uploaded ? uploaded : captured ? captured : started)));
}

static bool transient_control_error(int error)
{
  /* Only idempotent lease renewal/poll is retried. Never replay start,
   * upload, or a cloud operation with an ambiguous result. */
  return error == -ECONNABORTED || error == -ECONNRESET || error == -ENOTCONN || error == -ETIMEDOUT ||
         error == -EAGAIN || error == -EINTR || error == -EBUSY || error == -ENETDOWN ||
         error == -ENETUNREACH || error == -EHOSTUNREACH ||
         error == -ECONNREFUSED || error == -EPIPE;
}

static void *lease_worker(void *unused)
{
  unsigned int interval = g.vision_enabled ?
    (g.vision_long_poll ? 0 : AGORA_VISION_POLL_MS) : AGORA_KEEPALIVE_MS;
  uint64_t next = now_ms() + interval;
  (void)unused;
  while (!stopping(NULL))
    {
      if (now_ms() < next) { usleep(5000); continue; }
      struct agora_scene_request_s request = {0};
      uint64_t started = now_ms();
      int ret = g.vision_enabled ? agora_control_poll(g.local, g.lease, &request) :
                                  agora_control_keepalive(g.local, g.lease);
      uint64_t elapsed = now_ms() - started;
      if (stopping(NULL)) break; /* Expected lease cancellation is not a live poll failure. */
      nxmutex_lock(&g.lock);
      g.control_polls++;
      if (elapsed > g.control_max_ms) g.control_max_ms = elapsed;
      if (ret)
        { g.control_failures++; g.control_consecutive++; }
      else
        {
          if (g.control_consecutive) g.control_recovered++;
          g.control_consecutive = 0;
        }
      unsigned int failures = g.control_consecutive;
      nxmutex_unlock(&g.lock);
      if (ret)
        {
          control_failure(ret);
          if (!stopping(NULL) && transient_control_error(ret) &&
              failures < AGORA_CONTROL_MAX_FAILURES)
            {
              next = now_ms() + AGORA_CONTROL_RETRY_MS;
              continue;
            }
          if (!stopping(NULL)) fail(ret);
          break;
        }
      if (request.id[0] && !stopping(NULL)) observe_scene(request);
      next = now_ms() + interval;
    }
  return NULL;
}

/* Keep normal deadlines on the absolute 20ms grid. A late scheduler/SDK may
 * catch up at most two frames, with a minimum 10ms start-to-start gap; then rebase instead
 * of sending an unbounded burst. Queued audio is never silently discarded.
 */
static uint64_t advance_send(uint64_t due, uint64_t completed, unsigned int *late)
{
  uint64_t next = due + FRAME_MS;
  if (completed >= next)
    {
      if (++*late >= MAX_CATCHUP) { *late = 0; next = completed + FRAME_MS; }
    }
  else *late = 0;
  return next;
}

static uint64_t send_ready_at(uint64_t due, uint64_t previous_start)
{
  uint64_t earliest = previous_start ? previous_start + CATCHUP_GAP_MS : 0;
  return due > earliest ? due : earliest;
}

static int wait_send_deadline(uint64_t deadline)
{
  struct timespec wake =
    { .tv_sec = deadline / 1000u, .tv_nsec = (deadline % 1000u) * 1000000u };
  int ret;

  /* NuttX rounds relative sleeps up to scheduler ticks. An absolute
   * monotonic deadline does not add SDK execution time to every period. */
  do
    {
      if (stopping(NULL)) return 0;
      ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL);
    }
  while (ret == EINTR);
  return ret ? -ret : 0;
}

static int load_local(cJSON **root)
{
  char input[4096];
  int ret = agora_control_prepare_storage();
  if (ret < 0) return ret;
  FILE *file = fopen(AGORA_CONFIG, "r");
  if (!file) return -errno;
  size_t n = fread(input, 1, sizeof(input) - 1, file);
  bool overflow = fgetc(file) != EOF, io_error = ferror(file);
  fclose(file); input[n] = 0;
  if (overflow || io_error) ret = overflow ? -E2BIG : -EIO;
  else
    {
      *root = cJSON_ParseWithLengthOpts(input, n + 1, NULL, true);
      if (!cJSON_IsObject(*root)) ret = -EINVAL;
    }
  explicit_bzero(input, sizeof(input));
  return ret;
}
static int json_string(const cJSON *root, const char *name, char *out,
                       size_t minimum, size_t maximum)
{
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
  if (!cJSON_IsString(v) || !v->valuestring) return -EINVAL;
  size_t n = strlen(v->valuestring);
  if (n < minimum || n > maximum) return -EINVAL;
  memcpy(out, v->valuestring, n + 1);
  return 0;
}
static int json_uint(const cJSON *root, const char *name, uint32_t minimum,
                     uint32_t maximum, uint32_t *out)
{
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
  if (!cJSON_IsNumber(v) || !(v->valuedouble >= minimum && v->valuedouble <= maximum))
    return -EINVAL;
  uint32_t n = (uint32_t)v->valuedouble;
  if ((double)n != v->valuedouble) return -EINVAL;
  *out = n;
  return 0;
}
static int validate_config(const cJSON *root, bool init_only)
{
  uint32_t volume;
  if (json_string(root, "app_id", g.config.app_id, 32, 32)) return -EINVAL;
  for (size_t i = 0; i < 32; i++)
    if (!strchr("0123456789abcdefABCDEF", g.config.app_id[i])) return -EINVAL;
  if (init_only) return 0;
  if (json_string(root, "channel", g.config.channel, 1, 63) ||
      json_string(root, "token", g.config.token, 32, 512) ||
      json_uint(root, "uid", 1, UINT32_MAX, &g.config.uid) ||
      json_uint(root, "remote_uid", 1, UINT32_MAX, &g.config.remote_uid) ||
      json_uint(root, "volume", 0, 100, &volume) || g.config.uid == g.config.remote_uid)
    return -EINVAL;
  for (const unsigned char *p = (const unsigned char *)g.config.channel; *p; p++)
    if (*p < 32 || *p > 126 || strchr("\"'*/\\`", *p)) return -EINVAL;
  for (const unsigned char *p = (const unsigned char *)g.config.token; *p; p++)
    if (*p < 33 || *p > 126) return -EINVAL;
  g.config.volume = volume;
  return 0;
}

static void cleanup(void)
{
  int ret;
  bool cloud_closed = false;
  /* SDK 1.9.7 can keep rtc_inited set after init fails. Re-entering may
   * return 0 without creating a queue, then fini can hang. Do not invoke
   * fini on that partial state or free inputs it may still reference.
   * A reset is required; only a completed init is eligible for fini. */
  bool retain = g.sdk_init_failed;
  nxmutex_lock(&g.lock);
#ifdef CONFIG_CONTEST2026_441_WAKE
  retain = retain || g_wake_fault;
#endif
  g.stop = true; g.receive = false; g.phase = STOPPING;
  nxmutex_unlock(&g.lock);
  if (g.audio_started)
    {
      ret = pthread_join(g.audio, NULL);
      if (ret) { cleanup_failure(-ret); retain = true; }
      else g.audio_started = false;
    }
  /* A v2 poll may be waiting at the host. Cancel the lease before joining
   * that worker; its configuration stays alive until all workers finish.
   * Stop is idempotent and a failed early stop is retried below. */
  if (g.vision_long_poll && g.lease[0])
    {
      ret = agora_control_stop(g.local, g.lease);
      control_failure(ret);
      cloud_closed = ret == 0;
    }
  if (g.lease_started)
    {
      ret = pthread_join(g.lease_thread, NULL);
      if (ret) { cleanup_failure(-ret); retain = true; }
      else g.lease_started = false;
    }
  if (!retain)
    {
      if (g.connection)
        {
          cleanup_failure(agora_rtc_leave_channel(g.conn));
          ret = agora_rtc_destroy_connection(g.conn);
          cleanup_failure(ret);
          if (!ret)
            { nxmutex_lock(&g.lock); g.connection = false; nxmutex_unlock(&g.lock); }
        }
      if (g.initialized)
        {
          ret = agora_rtc_fini();
          cleanup_failure(ret);
          if (ret) retain = true;
          else
            {
              nxmutex_lock(&g.lock);
              g.initialized = false; g.connection = false;
              nxmutex_unlock(&g.lock);
            }
        }
    }
  /* Close cloud audio even when SDK cleanup failed. */
  if (g.lease[0] && !cloud_closed) control_failure(agora_control_stop(g.local, g.lease));
  if (!retain && (g.camera_paused || g.camera_live_owned))
    {
      char camera[160];
      ret = camera_service_control("resume", camera, sizeof(camera));
      if (ret) cleanup_failure(ret);
      /* A failed camera restart is reported, but is not an SDK retention. */
      nxmutex_lock(&g.lock);
      g.camera_paused = g.camera_live_owned = false;
      nxmutex_unlock(&g.lock);
    }
  nxmutex_lock(&g.lock);
  if (!retain)
    {
      bk7258_psram_free(g.tx.data);
      if (g.rx.data) explicit_bzero(g.rx.data, g.rx.capacity * sizeof(int16_t));
      if (g.rx_system) free(g.rx.data); else bk7258_psram_free(g.rx.data);
      g.rx_system = false;
      memset(&g.tx, 0, sizeof(g.tx)); memset(&g.rx, 0, sizeof(g.rx));
      if (g.session != g.local) cJSON_Delete(g.session);
      cJSON_Delete(g.local); g.session = g.local = NULL;
      explicit_bzero(&g.config, sizeof(g.config));
      explicit_bzero(g.lease, sizeof(g.lease));
    }
  g.joined = g.joining = g.playing = false;
#ifdef CONFIG_CONTEST2026_441_WAKE
  retain = retain || g_wake_fault;
#endif
  g.blocked = retain; g.phase = retain ? BLOCKED : IDLE; g.running = false;
  nxmutex_unlock(&g.lock);
}
static void *coordinator(void *unused)
{
  int ret;
  uint64_t deadline, next_send = 0, last_send = 0;
  unsigned int late = 0;
  uint32_t local_volume = 0;
  bool override_volume = false;
  int16_t pcm[PCM_SAMPLES];
  pthread_t audio;
  connection_id_t conn;
  (void)unused;
#ifdef CONFIG_CONTEST2026_441_WAKE
  deadline=now_ms()+5000;
  for (;;)
    {
      nxmutex_lock(&g.lock); bool busy=g_wake_busy, blocked=g.blocked; nxmutex_unlock(&g.lock);
      if (blocked) { fail(-EIO); goto done; }
      if (!busy) break;
      if (stopping(NULL)) goto done;
      if (now_ms()>=deadline) { fail(-ETIMEDOUT); goto done; }
      usleep(10000);
    }
#endif
  ret = step_result("config-read", load_local(&g.local));
  if (ret) { fail(ret); goto done; }
  if (stopping(NULL)) goto done;
  if (!g.init_only)
    {
      if (!g.silent)
        {
          char camera[160];
          ret = step_result(g.camera_live ? "camera-borrow" : "camera-pause",
                            camera_service_control(g.camera_live ? "borrow" : "pause",
                                                   camera, sizeof(camera)));
          if (ret && (g.camera_live || ret != -ENOSYS)) { fail(ret); goto done; }
          if (ret == 0)
            {
              nxmutex_lock(&g.lock);
              g.camera_live_owned = g.camera_live;
              g.camera_paused = !g.camera_live;
              nxmutex_unlock(&g.lock);
            }
        }
      if (cJSON_GetObjectItemCaseSensitive(g.local, "server_host"))
        {
          ret = json_uint(g.local, "volume", 0, 100, &local_volume);
          if (ret) { fail(ret); goto done; }
          override_volume = true;
        }
      ret = step_result("network", mimo_http_prepare_network());
      if (ret) { fail(ret); goto done; }
      if (stopping(NULL)) goto done;
      if (cJSON_GetObjectItemCaseSensitive(g.local, "server_host"))
        {
          ret = step_result("controller-start", agora_control_start(g.local, &g.session, g.lease));
          if (ret) { control_failure(ret); fail(ret); goto done; }
        }
      else g.session = g.local;
    }
  ret = step_result("config-check", validate_config(g.init_only ? g.local : g.session, g.init_only));
  if (ret) { fail(ret); goto done; }
  if (override_volume) g.config.volume = local_volume;
  cJSON *vision = cJSON_GetObjectItemCaseSensitive(g.session, "vision_tools");
  if (vision && !cJSON_IsBool(vision)) { fail(-EPROTO); goto done; }
  if (cJSON_IsTrue(vision))
    {
      if (g.silent || (!g.camera_paused && !g.camera_live_owned) || !g.lease[0])
        { fail(-EPROTO); goto done; }
      g.vision_enabled = true;
    }
  cJSON *long_poll = cJSON_GetObjectItemCaseSensitive(g.session, "vision_long_poll");
  if (long_poll && (!cJSON_IsBool(long_poll) ||
                   (cJSON_IsTrue(long_poll) && !g.vision_enabled)))
    { fail(-EPROTO); goto done; }
  g.vision_long_poll = cJSON_IsTrue(long_poll);
  if (stopping(NULL)) goto done;
  if (!g.silent)
    {
      g.tx.data = bk7258_psram_malloc(TX_SAMPLES * sizeof(int16_t));
      g.rx_system = g.vision_enabled;
      g.rx.capacity = g.vision_enabled ? RX_VISION_SAMPLES : RX_SAMPLES;
      g.rx.data = g.rx_system ? malloc(g.rx.capacity * sizeof(int16_t)) :
                              bk7258_psram_malloc(g.rx.capacity * sizeof(int16_t));
      g.tx.capacity = TX_SAMPLES;
      if (!g.tx.data || !g.rx.data) { fail(-ENOMEM); goto done; }
    }
  phase(INITIALIZING);
  g.service_options.log_cfg.log_disable = true;
  ret = step_result("sdk-init", agora_rtc_init(g.config.app_id, &g_events, &g.service_options));
  sdk_failure(ret);
  if (ret)
    {
      nxmutex_lock(&g.lock); g.sdk_init_failed = true; nxmutex_unlock(&g.lock);
      goto done;
    }
  nxmutex_lock(&g.lock); g.initialized = true; nxmutex_unlock(&g.lock);
  if (g.init_only || stopping(NULL)) goto done;
  ret = step_result("sdk-create", agora_rtc_create_connection(&conn));
  sdk_failure(ret);
  if (ret) goto done;
  nxmutex_lock(&g.lock);
  g.conn = conn; g.connection = g.joining = true; g.phase = JOINING;
  nxmutex_unlock(&g.lock);
  if (stopping(NULL)) goto done;
  g.channel_options.auto_subscribe_audio = !g.silent;
  g.channel_options.enable_audio_jitter_buffer = true;
  g.channel_options.enable_audio_decode = true;
  g.channel_options.audio_codec_opt.audio_codec_type = AUDIO_CODEC_TYPE_G722;
  g.channel_options.audio_codec_opt.pcm_sample_rate = 16000;
  g.channel_options.audio_codec_opt.pcm_channel_num = 1;
  g.channel_options.audio_codec_opt.pcm_duration = 20;
  ret = step_result("sdk-join", agora_rtc_join_channel(conn, g.config.channel, g.config.uid,
                              g.config.token, &g.channel_options));
  sdk_failure(ret);
  if (ret) goto done;
  deadline = now_ms() + AGORA_JOIN_MS;
  while (!stopping(NULL))
    {
      bool joined;
      nxmutex_lock(&g.lock); joined = g.joined; nxmutex_unlock(&g.lock);
      if (joined) break;
      if (now_ms() >= deadline) { fail(-ETIMEDOUT); goto done; }
      usleep(5000);
    }
  if (stopping(NULL)) goto done;
  nxmutex_lock(&g.lock);
  g.phase = ACTIVE; g.joining = false; g.receive = !g.silent;
  nxmutex_unlock(&g.lock);
  if (g.lease[0])
    {
      ret = create_worker(&g.lease_thread, lease_worker, false, LEASE_PRIORITY);
      if (ret) { fail(ret); goto done; }
      g.lease_started = true;
    }
  if (!g.silent)
    {
      ret = create_worker(&audio, audio_worker, false, AUDIO_PRIORITY);
      if (ret) { fail(ret); goto done; }
      g.audio = audio; g.audio_started = true;
    }
  deadline = now_ms() + AGORA_PROBE_MS;
  while (!stopping(NULL))
    {
      uint64_t now = now_ms();
      if (g.silent && now >= deadline) break;
      bool send = false;
      uint64_t wake = now + CATCHUP_GAP_MS;
      nxmutex_lock(&g.lock);
      if (g.playing || g.observing) { next_send = last_send = 0; late = 0; }
      if (!g.stop && !g.silent && !g.playing && !g.observing && g.tx.count >= PCM_SAMPLES)
        {
          wake = send_ready_at(next_send, last_send);
          if (now >= wake)
            {
              if (!next_send) next_send = now;
              queue_get(&g.tx, pcm, PCM_SAMPLES); send = true;
            }
        }
      nxmutex_unlock(&g.lock);
      if (send)
        {
          audio_frame_info_t info = { .data_type = AUDIO_DATA_TYPE_PCM };
          uint64_t clipped = 0;
          unsigned int peak = scale_tx_pcm(pcm, PCM_SAMPLES, g.mic_gain, &clipped);
          nxmutex_lock(&g.lock);
          if (peak > g.mic_tx_peak) g.mic_tx_peak = peak;
          g.mic_clipped += clipped;
          nxmutex_unlock(&g.lock);
          uint64_t started = now_ms();
          ret = agora_rtc_send_audio_data(conn, pcm, sizeof(pcm), &info);
          uint64_t completed = now_ms();
          nxmutex_lock(&g.lock);
          g.send_calls++;
          if (completed - started > g.send_max_ms)
            g.send_max_ms = completed - started;
          if (g.last_send_started && started - g.last_send_started > g.send_gap_max_ms)
            g.send_gap_max_ms = started - g.last_send_started;
          g.last_send_started = started;
          nxmutex_unlock(&g.lock);
          sdk_failure(ret);
          if (ret) break;
          nxmutex_lock(&g.lock); g.up += PCM_SAMPLES; nxmutex_unlock(&g.lock);
          last_send = started;
          next_send = advance_send(next_send, completed, &late);
          wake = send_ready_at(next_send, last_send);
        }
      ret = wait_send_deadline(wake);
      if (ret) { fail(ret); break; }
    }
done:
  explicit_bzero(pcm, sizeof(pcm));
  cleanup();
  return NULL;
}

#ifdef CONFIG_CONTEST2026_441_POWER
static bool g_power_inhibited;

int agora_service_power_quiesce(void)
{
  int ret;
  nxmutex_lock(&g.lock);
  g_power_inhibited = true;
  g.stop = true;
  ret = g.blocked ? -EIO :
         (g.running || g.audio_started || g.lease_started
#ifdef CONFIG_CONTEST2026_441_WAKE
          || g_wake_busy
#endif
         ) ? -EAGAIN :
        (g.initialized || g.connection || g.camera_paused || g.camera_live_owned) ? -EIO : 0;
  nxmutex_unlock(&g.lock);
  return ret;
}

int agora_service_power_resume(void)
{
  int ret;
  nxmutex_lock(&g.lock);
  ret = (g.blocked || g.initialized || g.connection) ? -EIO :
        (g.running || g.audio_started || g.lease_started || g.camera_paused || g.camera_live_owned
#ifdef CONFIG_CONTEST2026_441_WAKE
         || g_wake_busy
#endif
        ) ? -EBUSY : 0;
  if (!ret) g_power_inhibited = false;
  nxmutex_unlock(&g.lock);
  return ret;
}
#endif

#ifdef CONFIG_CONTEST2026_441_WAKE
int agora_service_wake_acquire(void)
{
  nxmutex_lock(&g.lock);
  int ret=g.blocked || g.initialized || g.connection ? -EIO :
          g.running || g_wake_busy ? -EBUSY : 0;
#ifdef CONFIG_CONTEST2026_441_POWER
  if (g_power_inhibited) ret=-ESHUTDOWN;
#endif
  if (!ret) g_wake_busy=true;
  nxmutex_unlock(&g.lock);
  return ret;
}
bool agora_service_wake_cancelled(void)
{
  nxmutex_lock(&g.lock);
  bool cancel=g.running || g.blocked;
#ifdef CONFIG_CONTEST2026_441_POWER
  cancel=cancel || g_power_inhibited;
#endif
  nxmutex_unlock(&g.lock);
  return cancel;
}
void agora_service_wake_release(int device_error)
{
  nxmutex_lock(&g.lock);
  g_wake_busy=false;
  if (device_error<0) { g_wake_fault=true; g.blocked=true; g.error=device_error; g.phase=BLOCKED; }
  nxmutex_unlock(&g.lock);
}
#endif

int agora_service_command(const char *action, char *text, size_t capacity)
{
  static const char *const phases[] =
    { "idle", "config", "init", "joining", "active", "stopping", "blocked" };
  pthread_t thread;
  int ret = 0;
  if (!action || !text || !capacity) return -EINVAL;
#ifdef CONFIG_CONTEST2026_441_WAKE
  if (!strncmp(action,"wake-",5)) return wake_service_command(action+5,text,capacity);
#endif
  text[0] = 0;
  if (!strcmp(action, "version"))
    { snprintf(text, capacity, "Agora SDK=%s", agora_rtc_get_version()); return 0; }
  nxmutex_lock(&g.lock);
  if (!strcmp(action, "stats"))
    snprintf(text, capacity,
      "Agora stats captured=%llu sent=%llu elapsed=%llu ms chunk_max=%lu tx_high=%lu "
      "send_calls=%u send_max=%llu ms gap_max=%llu ms overflow_tx/rx=%u/%u",
      (unsigned long long)g.captured, (unsigned long long)g.up,
      (unsigned long long)g.tx_elapsed, (unsigned long)g.capture_max,
      (unsigned long)g.tx_high, g.send_calls, (unsigned long long)g.send_max_ms,
      (unsigned long long)g.send_gap_max_ms, g.tx_overflows, g.rx_overflows);
  else if (!strcmp(action, "link"))
    snprintf(text, capacity,
      "Agora link remote_seen=%u expected_seen=%u rx_seen=%u uid_drop=%u state_drop=%u "
      "mic_peak=%u mic_mean_max=%u (waveform only, not speech recognition)",
      g.remote_seen, g.expected_seen, g.rx_seen, g.rx_uid_drop, g.rx_state_drop,
      g.mic_peak, g.mic_mean_max);
  else if (!strcmp(action, "micstats"))
    snprintf(text, capacity,
      "Agora MIC raw_peak=%u raw_mean_max=%u gain=%u prepared_peak=%u clipped=%llu "
      "(prepared PCM only, not ASR)",
      g.mic_peak, g.mic_mean_max, g.mic_gain, g.mic_tx_peak,
      (unsigned long long)g.mic_clipped);
  else if (!strcmp(action, "visionstats"))
    snprintf(text, capacity,
      "Agora vision enabled=%d observing=%d quiesced=%d requests=%u completed=%u failed=%u error=%d flushed=%llu",
      g.vision_enabled, g.observing, g.audio_quiesced, g.vision_requests,
      g.vision_completed, g.vision_failed, g.vision_error,
      (unsigned long long)g.vision_flushed);
  else if (!strcmp(action, "controlstats"))
    snprintf(text, capacity,
      "Agora control polls=%u failures=%u consecutive=%u recovered=%u max_ms=%llu last_error=%d long_poll=%d",
      g.control_polls, g.control_failures, g.control_consecutive, g.control_recovered,
      (unsigned long long)g.control_max_ms, g.control_error, g.vision_long_poll);
  else if (!strcmp(action, "mic-gain"))
    snprintf(text, capacity, "Agora microphone gain=%u; RAM only; no audio", g.mic_gain);
  else if (!strcmp(action, "camera-live-status") || !strcmp(action, "camera-live-on") ||
           !strcmp(action, "camera-live-off"))
    {
      if (strcmp(action, "camera-live-status"))
        {
          if (g.running || g.audio_started || g.lease_started) ret = -EBUSY;
          else if (g.blocked || g.initialized || g.connection || g.camera_live_owned || g.camera_paused)
            ret = -EIO;
          else g_camera_live_requested = !strcmp(action, "camera-live-on");
        }
      snprintf(text, capacity, "Agora camera mode=%s active_live=%d ret=%d; RAM only; no Audio/cloud",
               g_camera_live_requested ? "live" : "handoff", g.camera_live_owned, ret);
    }
  else if (!strncmp(action, "mic-gain ", 9))
    {
      char *end;
      unsigned long gain = strtoul(action + 9, &end, 10);
      if (action[9] < '1' || action[9] > '9' || *end || gain < 1 || gain > 16)
        ret = -EINVAL;
      else if (g.running || g.audio_started || g.lease_started) ret = -EBUSY;
      else if (g.blocked || g.initialized || g.connection) ret = -EIO;
      else g.mic_gain = (unsigned int)gain;
      snprintf(text, capacity, "Agora microphone gain=%u ret=%d; RAM only; no audio",
               g.mic_gain, ret);
    }
  else if (!strcmp(action, "netstats")) bk7258_agora_netstats(text, capacity);
  else if (!strcmp(action, "rxcache"))
    snprintf(text, capacity,
      "Agora RX cache samples=%lu/%lu high=%lu max_chunk=%lu heap=%s",
      (unsigned long)g.rx.count, (unsigned long)g.rx.capacity,
      (unsigned long)g.rx_high, (unsigned long)g.rx_largest,
      !g.rx.data ? "none" : g.rx_system ? "system" : "sdk");
  else if (!strcmp(action, "rxcache-test"))
    {
      if (g.running || g.blocked || g.initialized || g.connection ||
          g.audio_started || g.lease_started) ret = -EBUSY;
      else
        {
          size_t bytes = RX_VISION_SAMPLES * sizeof(int16_t);
          void *pcm = malloc(bytes);
          void *image = pcm ? malloc(AGORA_VISION_BYTES) : NULL;
          ret = pcm && image ? 0 : -ENOMEM;
          if (image) { explicit_bzero(image,AGORA_VISION_BYTES); free(image); }
          if (pcm) { explicit_bzero(pcm,bytes); free(pcm); }
        }
      snprintf(text,capacity,
        "Agora RX reserve probe rx=%lu image=%u ret=%d; allocations freed; no Audio/cloud",
        (unsigned long)(RX_VISION_SAMPLES*sizeof(int16_t)),AGORA_VISION_BYTES,ret);
    }
  else if (!strcmp(action, "rxstats"))
    snprintf(text, capacity,
      "Agora RX callbacks=%u peak_max=%u last_peak=%u zero=%u quiet=%u ignored=%u floor=%u "
      "playing=%d queued=%lu voiced_age_ms=%llu (PCM amplitude, not VAD)",
      g.rx_seen, g.rx_peak, g.rx_last_peak, g.rx_zero_frames, g.rx_quiet_frames,
      g.rx_quiet_ignored, AGORA_RX_SILENCE_PEAK,
      g.playing, (unsigned long)g.rx.count,
      (unsigned long long)(g.last_rx ? now_ms() - g.last_rx : 0));
  else if (!strcmp(action, "status"))
    snprintf(text, capacity,
      "Agora %s run=%d joined=%d silent=%d up=%llu down=%llu tx=%lu rx=%lu "
      "error=%d sdk=%d buffer=%d audio=%d cleanup=%d control=%d retained=%d camera_paused=%d",
      phases[g.phase], g.running, g.joined, g.silent,
      (unsigned long long)g.up, (unsigned long long)g.down,
      (unsigned long)g.tx.count, (unsigned long)g.rx.count, g.error, g.sdk_error,
      g.buffer_error, g.audio_error, g.cleanup_error, g.control_error, g.blocked,
      g.camera_paused);
  else if (!strncmp(action, "config ", 7))
    {
      if (g.running) ret = -EBUSY;
      else if (g.blocked || g.initialized || g.connection) ret = -EIO;
      else if (g.audio_started || g.lease_started) ret = -EBUSY;
      else ret = agora_control_save(action + 7);
      snprintf(text, capacity, ret ? "LAN configuration save failed ret=%d" :
               "LAN configuration saved on AP; no audio started", ret);
    }
  else if (!strcmp(action, "stop") || (!strcmp(action, "toggle") && g.running))
    { g.stop = true; snprintf(text, capacity, "Agora stop requested; cleanup=async retained=%d", g.blocked); }
  else if (!strcmp(action, "start") || !strcmp(action, "toggle") ||
           !strcmp(action, "probe") || !strcmp(action, "init-test"))
    {
#ifdef CONFIG_CONTEST2026_441_POWER
      if (g_power_inhibited) ret = -ESHUTDOWN;
      else
#endif
      if (g.blocked) ret = -EIO;
      else if (g.running) ret = -EBUSY;
      else
        {
          g.stop = false; g.silent = !strcmp(action, "probe") || !strcmp(action, "init-test");
          g.init_only = !strcmp(action, "init-test");
          g.camera_live = g_camera_live_requested && !g.silent;
          g.vision_enabled = g.vision_long_poll = g.observing = g.audio_quiesced = false;
          memset(g.vision_last, 0, sizeof(g.vision_last));
          g.vision_requests = g.vision_completed = g.vision_failed = 0;
          g.vision_error = 0; g.vision_flushed = 0;
          g.error = g.sdk_error = g.buffer_error = g.audio_error = 0;
          g.cleanup_error = g.control_error = 0; g.up = g.down = 0; g.conn = 0;
          g.control_polls = g.control_failures = g.control_consecutive = g.control_recovered = 0;
          g.control_max_ms = 0;
          g.captured = g.tx_started = g.tx_elapsed = g.last_send_started = 0;
          g.send_max_ms = g.send_gap_max_ms = 0;
          g.tx_high = g.capture_max = 0;
          g.send_calls = g.tx_overflows = g.rx_overflows = 0;
          g.rx_high = g.rx_largest = 0;
          g.remote_seen = g.expected_seen = g.rx_seen = g.rx_uid_drop = g.rx_state_drop = 0;
          g.mic_peak = g.mic_mean_max = 0;
          g.mic_tx_peak = 0; g.mic_clipped = 0;
          g.rx_peak = g.rx_last_peak = g.rx_zero_frames = 0;
          g.response_rx_ms=0; g.response_number=0;
          g.rx_quiet_frames = g.rx_quiet_ignored = 0;
          g.last_rx = 0;
          bk7258_agora_netreset();
          memset(&g.service_options, 0, sizeof(g.service_options));
          memset(&g.channel_options, 0, sizeof(g.channel_options));
          g.phase = CONFIGURING; g.running = true;
          ret = create_worker(&thread, coordinator, true, TX_PRIORITY);
          if (ret) { g.running = false; g.phase = IDLE; g.error = ret; }
        }
      snprintf(text, capacity, "Agora %s queued=%d ret=%d silent=%d", action, ret == 0, ret, g.silent);
    }
  else ret = -EINVAL;
  nxmutex_unlock(&g.lock);
  return ret;
}
