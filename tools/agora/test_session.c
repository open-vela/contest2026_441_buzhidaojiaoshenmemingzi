/* SPDX-License-Identifier: Apache-2.0 */
/* Real session implementation with the pinned SDK header and mock I/O. */
#include <assert.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
static int mock_attr_inherit(pthread_attr_t *attr, int inherit);
static int mock_attr_policy(pthread_attr_t *attr, int policy);
static int mock_attr_priority(pthread_attr_t *attr, const struct sched_param *param);
static atomic_int tx_sched, audio_sched, lease_sched;
static int reject_priority;
#define pthread_attr_setinheritsched mock_attr_inherit
#define pthread_attr_setschedpolicy mock_attr_policy
#define pthread_attr_setschedparam mock_attr_priority
_Thread_local int r1_test_lock_depth;
static int mock_join(pthread_t thread, void **result);
#define pthread_join mock_join
#define VISION_AGORA_HOST_TEST 1
#define AGORA_CONFIG "/tmp/r1-agora-session-test.cfg"
#define AGORA_JOIN_MS 100u
#define AGORA_PROBE_MS 80u
#ifndef AGORA_RX_IDLE_MS
#define AGORA_RX_IDLE_MS 30u
#endif
#define AGORA_KEEPALIVE_MS 40u
#define AGORA_VISION_POLL_MS 40u
#define AGORA_CONTROL_RETRY_MS 40u
#define CONFIG_BK7258_AIDK_EYE_LVGL_STATUS 1
#define CONFIG_CONTEST2026_441_POWER 1
#define CONFIG_CONTEST2026_441_WAKE 1
static void *mock_app_malloc(size_t size);
static void mock_app_free(void *pointer);
#define malloc mock_app_malloc
#define free mock_app_free
#include "../../app/vision_badge/src/agora_service.c"
#undef malloc
#undef free
#undef pthread_join
#undef pthread_attr_setinheritsched
#undef pthread_attr_setschedpolicy
#undef pthread_attr_setschedparam

static int wake_delegations;
#ifndef R1_WAKE_INTEGRATION
int wake_service_command(const char *action,char *text,size_t capacity)
{
  assert(!r1_test_lock_depth);
  assert(!strcmp(action,"start") || !strcmp(action,"stop") || !strcmp(action,"status"));
  wake_delegations++;snprintf(text,capacity,"wake mock");return 0;
}
#endif

static atomic_int last_activity;
static atomic_uint activity_seen;
static unsigned int feedback_delay_us;
void feedback_service_activity(enum vision_badge_stage_e stage)
{
  assert(!r1_test_lock_depth); /* Display feedback must not hold session locks. */
  assert(stage >= VISION_BADGE_STAGE_IDLE && stage <= VISION_BADGE_STAGE_ERROR);
  atomic_store(&last_activity, stage);
  atomic_fetch_or(&activity_seen, 1u << stage);
  if (stage==VISION_BADGE_STAGE_QUERY && feedback_delay_us) usleep(feedback_delay_us);
}

/* Exercise the production attribute calls, without requiring Linux RT
 * privileges or applying NuttX's priority120/125 to Linux's1..99 range. */
static int mock_attr_inherit(pthread_attr_t *attr, int inherit)
{ assert(attr && inherit == PTHREAD_EXPLICIT_SCHED); return 0; }
static int mock_attr_policy(pthread_attr_t *attr, int policy)
{ assert(attr && policy == SCHED_RR); return 0; }
static int mock_attr_priority(pthread_attr_t *attr, const struct sched_param *param)
{
  assert(attr && param);
  if (param->sched_priority == reject_priority) return EPERM;
  switch (param->sched_priority)
    {
      case TX_PRIORITY: atomic_fetch_add(&tx_sched, 1); break;
      case AUDIO_PRIORITY: atomic_fetch_add(&audio_sched, 1); break;
      case LEASE_PRIORITY: atomic_fetch_add(&lease_sched, 1); break;
#ifdef R1_WAKE_INTEGRATION
      case 115: break;
#endif
      default: assert(!"unexpected worker priority");
    }
  return 0;
}

static const char *valid = "{\"app_id\":\"0123456789abcdef0123456789abcdef\","
  "\"channel\":\"mock-room\",\"uid\":123,\"remote_uid\":456,"
  "\"token\":\"01234567890123456789012345678901\",\"volume\":30}";
static struct
{
  atomic_int allocations, init, fini, join, leave, destroy, send;
  atomic_int capture, play, owners, complete, cloud_start, cloud_stop, keepalive;
  atomic_int camera_pause, camera_resume, camera_borrow;
  atomic_bool live_release;
  atomic_int scene_number, scene_capture, scene_upload, scene_notify;
  atomic_bool draining, keepalive_busy;
  atomic_bool http_release;
  atomic_int keepalive_complete, playback_volume, played_samples, tiny_samples;
  atomic_int keepalive_once;
  atomic_int scene_allocations, sdk_scene_attempts;
  atomic_int rx_allocations, sdk_rx_attempts, guard_samples;
  void *scene_buffer;
  void *rx_buffer;
  int init_error, fini_error, keepalive_error, send_error, join_error;
  int camera_pause_error, camera_resume_error;
  int scene_camera_error, scene_http_error;
  bool vision_mode, long_poll, scene_alloc_fail, capture_stuck, burst_pattern, rx_alloc_fail, pre_join_audio, http_burst;
  unsigned int scene_delay_us, scene_remaining_ms;
  unsigned int keepalive_delay_us, send_delay_us, expected_volume, expected_gain;
  bool withhold_join, tx_overflow, lease_join_error, early_audio, tiny_pcm, zero_pcm;
  bool codec_silence;
  pthread_t coordinator;
  const agora_rtc_event_handler_t *events;
  const char *held_app;
  const char *held_token;
} mock;
static unsigned int net_resets;
int camera_service_control(const char *action, char *text, size_t capacity)
{
  assert(r1_test_lock_depth == 0 && atomic_load(&mock.owners) == 0);
  snprintf(text, capacity, "camera mock");
  if (!strcmp(action, "pause"))
    { atomic_fetch_add(&mock.camera_pause, 1); return mock.camera_pause_error; }
  if (!strcmp(action, "borrow"))
    { atomic_fetch_add(&mock.camera_borrow, 1); return mock.camera_pause_error; }
  assert(!strcmp(action, "resume"));
  atomic_fetch_add(&mock.camera_resume, 1);
  return mock.camera_resume_error;
}
void bk7258_agora_netreset(void) { net_resets++; }
void bk7258_agora_netstats(char *text, size_t capacity)
{ snprintf(text, capacity, "Agora net mocked counters"); }
static int mock_join(pthread_t thread, void **result)
{
  if (mock.lease_join_error && pthread_equal(thread, g.lease_thread)) return EINVAL;
  return mock.join_error ? mock.join_error : pthread_join(thread, result);
}

static void unlocked(void)
{
  /* Other workers can legitimately own the mutex. Check this caller, not
   * whether another thread happens to be inside its short queue operation.
   */
  assert(r1_test_lock_depth == 0);
}
static void rtc_owner(void)
{
  unlocked();
  assert(pthread_equal(mock.coordinator, pthread_self()));
}
void *bk7258_psram_malloc(size_t size)
{
  /* R1 SDK private heap has <256KiB free even at idle, while the NuttX
   * system heap has an independent multi-MiB region. Never conflate them. */
  if (size == AGORA_VISION_BYTES)
    { atomic_fetch_add(&mock.sdk_scene_attempts, 1); return NULL; }
  if (size == RX_VISION_SAMPLES * sizeof(int16_t))
    { atomic_fetch_add(&mock.sdk_rx_attempts, 1); return NULL; }
  void *p = malloc(size);
  if (p) atomic_fetch_add(&mock.allocations, 1);
  return p;
}
void bk7258_psram_free(void *p)
{
  if (p) { atomic_fetch_sub(&mock.allocations, 1); free(p); }
}
static void *mock_app_malloc(size_t size)
{
  if (size == RX_VISION_SAMPLES*sizeof(int16_t) && mock.rx_alloc_fail) return NULL;
  if (size == AGORA_VISION_BYTES && mock.scene_alloc_fail) return NULL;
  void *pointer = malloc(size);
  if (pointer && size == RX_VISION_SAMPLES * sizeof(int16_t))
    {
      assert(!mock.rx_buffer); mock.rx_buffer = pointer;
      atomic_fetch_add(&mock.rx_allocations, 1);
    }
  if (pointer && size == AGORA_VISION_BYTES)
    {
      assert(!mock.scene_buffer);
      mock.scene_buffer = pointer;
      atomic_fetch_add(&mock.scene_allocations, 1);
    }
  return pointer;
}
static void mock_app_free(void *pointer)
{
  if (pointer && pointer == mock.rx_buffer)
    {
      const unsigned char *bytes = pointer;
      for (size_t i=0; i<RX_VISION_SAMPLES*sizeof(int16_t); i++) assert(bytes[i] == 0);
      mock.rx_buffer = NULL; atomic_fetch_sub(&mock.rx_allocations, 1);
    }
  if (pointer && pointer == mock.scene_buffer)
    {
      const unsigned char *bytes = pointer;
      for (size_t i=0; i<AGORA_VISION_BYTES; i++) assert(bytes[i] == 0);
      mock.scene_buffer = NULL;
      atomic_fetch_sub(&mock.scene_allocations, 1);
    }
  free(pointer);
}
const char *agora_rtc_get_version(void) { return "mock-pinned-api"; }
int agora_rtc_init(const char *app, const agora_rtc_event_handler_t *events,
                   rtc_service_option_t *options)
{
  unlocked();
  mock.coordinator = pthread_self();
  mock.events = events; mock.held_app = app;
  atomic_fetch_add(&mock.init, 1);
  assert(strlen(app) == 32 && options->log_cfg.log_disable);
  return mock.init_error;
}
int agora_rtc_create_connection(connection_id_t *conn)
{ rtc_owner(); *conn = 42; return 0; }
int agora_rtc_join_channel(connection_id_t conn, const char *channel, uint32_t uid,
                           const char *token, rtc_channel_options_t *options)
{
  rtc_owner();
  assert(conn == 42 && !strcmp(channel, "mock-room") && uid == 123);
  assert(strlen(token) == 32);
  assert(options->auto_subscribe_audio == !g.silent && options->enable_audio_jitter_buffer &&
         options->enable_audio_decode && !options->auto_subscribe_video &&
         !options->enable_audio_mixer && !options->enable_audio_downlink_aec);
  assert(options->audio_codec_opt.audio_codec_type == AUDIO_CODEC_TYPE_G722 &&
         options->audio_codec_opt.pcm_sample_rate == 16000 &&
         options->audio_codec_opt.pcm_channel_num == 1 &&
         options->audio_codec_opt.pcm_duration == 20);
  mock.held_token = token;
  atomic_fetch_add(&mock.join, 1);
  if (mock.pre_join_audio)
    {
      int16_t pcm[PCM_SAMPLES];
      for (size_t i=0; i<PCM_SAMPLES; i++) pcm[i]=123;
      audio_frame_info_t info = { .data_type=AUDIO_DATA_TYPE_PCM };
      assert(g.joining && !g.joined);
      mock.events->on_audio_data(42,456,0,pcm,sizeof(pcm),&info);
      assert(g.rx.count==PCM_SAMPLES && g.down==PCM_SAMPLES);
      assert(!atomic_load(&mock.play));
    }
  if (!mock.withhold_join)
    {
      mock.events->on_reconnecting(42);
      assert(!g.stop && g.error == 0);
      mock.events->on_join_channel_success(42, 123, 2);
      if (mock.early_audio)
        {
          int16_t pcm[640];
          for (size_t i = 0; i < 640; i++) pcm[i] = 123;
          audio_frame_info_t info = {.data_type = AUDIO_DATA_TYPE_PCM};
          /* Real SDK may deliver greeting before join_channel returns. */
          mock.events->on_audio_data(42, 456, 0, pcm, sizeof(pcm), &info);
        }
    }
  return 0;
}
int agora_rtc_send_audio_data(connection_id_t conn, const void *pcm, size_t bytes,
                             audio_frame_info_t *info)
{
  rtc_owner();
  assert(conn == 42 && bytes == PCM_SAMPLES * 2 && info->data_type == AUDIO_DATA_TYPE_PCM);
  const int16_t *samples = pcm;
  for (size_t i = 0; i < PCM_SAMPLES; i++)
    assert(samples[i] == 321 * (int)(mock.expected_gain ? mock.expected_gain : 16));
  atomic_fetch_add(&mock.send, 1);
  if (mock.send_delay_us) usleep(mock.send_delay_us);
  return mock.send_error;
}
int agora_rtc_leave_channel(connection_id_t conn)
{ rtc_owner(); assert(conn == 42); atomic_fetch_add(&mock.leave, 1); return 0; }
int agora_rtc_destroy_connection(connection_id_t conn)
{ rtc_owner(); assert(conn == 42); atomic_fetch_add(&mock.destroy, 1); return 0; }
int agora_rtc_fini(void)
{ rtc_owner(); atomic_fetch_add(&mock.fini, 1); return mock.fini_error; }

int mimo_http_prepare_network(void) { unlocked(); return 0; }
int agora_control_prepare_storage(void) { return 0; }
int agora_control_save(const char *json)
{
  cJSON *root = cJSON_Parse(json);
  if (!cJSON_IsObject(root)) { cJSON_Delete(root); return -EINVAL; }
  FILE *file = fopen(AGORA_CONFIG, "w"); assert(file);
  assert(fputs(json, file) >= 0); assert(fclose(file) == 0);
  cJSON_Delete(root);
  return 0;
}
int agora_control_start(const cJSON *local, cJSON **session, char lease[65])
{
  unlocked(); assert(cJSON_IsObject(local));
  atomic_fetch_add(&mock.cloud_start, 1);
  *session = cJSON_Parse(valid); assert(*session);
  assert(cJSON_ReplaceItemInObjectCaseSensitive(*session, "volume", cJSON_CreateNumber(55)));
  if (mock.vision_mode) assert(cJSON_AddBoolToObject(*session, "vision_tools", true));
  if (mock.long_poll) assert(cJSON_AddBoolToObject(*session, "vision_long_poll", true));
  strcpy(lease, "mock-lease"); return 0;
}
int agora_control_keepalive(const cJSON *local, const char *lease)
{
  unlocked();
  assert(!pthread_equal(mock.coordinator, pthread_self()));
  assert(cJSON_IsObject(local) && !strcmp(lease, "mock-lease"));
  atomic_store(&mock.keepalive_busy, true);
  atomic_fetch_add(&mock.keepalive, 1);
  if (mock.keepalive_delay_us) usleep(mock.keepalive_delay_us);
  atomic_store(&mock.keepalive_busy, false);
  atomic_fetch_add(&mock.keepalive_complete, 1);
  int once = atomic_exchange(&mock.keepalive_once, 0);
  return once ? once : mock.keepalive_error;
}
int agora_control_stop(const cJSON *local, const char *lease)
{
  unlocked(); assert(cJSON_IsObject(local) && !strcmp(lease, "mock-lease"));
  atomic_fetch_add(&mock.cloud_stop, 1);
  if (mock.long_poll)
    {
      mock.events->on_user_offline(g.conn, g.config.remote_uid, 0);
      mock.events->on_connection_lost(g.conn);
    }
  return 0;
}
int agora_control_poll(const cJSON *local, const char *lease,
                       struct agora_scene_request_s *request)
{
  int ret = agora_control_keepalive(local, lease);
  if (mock.long_poll)
    while (!atomic_load(&mock.cloud_stop) && !atomic_load(&mock.scene_number)) usleep(1000);
  memset(request, 0, sizeof(*request));
  int number = atomic_load(&mock.scene_number);
  if (!ret && number)
    {
      snprintf(request->id, sizeof(request->id), "%032x", number);
      request->remaining_ms = mock.scene_remaining_ms ? mock.scene_remaining_ms : 2000;
    }
  return ret;
}
int camera_service_capture_paused(struct vision_badge_image_s *image)
{
  unlocked();
  assert(atomic_load(&mock.owners) == 0 && !atomic_load(&mock.draining));
  nxmutex_lock(&g.lock);
  assert(g.observing && g.audio_quiesced && g.camera_paused);
  nxmutex_unlock(&g.lock);
  atomic_fetch_add(&mock.scene_capture, 1);
  if (mock.scene_delay_us) usleep(mock.scene_delay_us);
  if (mock.scene_camera_error) return mock.scene_camera_error;
  assert(image->capacity >= 6);
  memcpy(image->data, "\xff\xd8xx\xff\xd9", 6);
  image->size = 6;image->width = 640;image->height = 480;
  return 0;
}
int camera_service_capture_live(struct vision_badge_image_s *image,
                                unsigned int timeout_ms,
                                bool (*cancel)(void *), void *arg)
{
  unlocked();
  assert(g.camera_live_owned && !g.camera_paused && !g.observing && !g.audio_quiesced);
  atomic_fetch_add(&mock.scene_capture,1);
  uint64_t deadline=now_ms()+timeout_ms;
  while (!atomic_load(&mock.live_release))
    {
      if (cancel && cancel(arg)) return -ECANCELED;
      if (now_ms()>=deadline) return -ETIMEDOUT;
      usleep(1000);
    }
  if (cancel && cancel(arg)) return -ECANCELED;
  if (now_ms()>=deadline) return -ETIMEDOUT;
  if (mock.scene_camera_error) return mock.scene_camera_error;
  assert(image->capacity>=6);
  memcpy(image->data,"\xff\xd8xx\xff\xd9",6);
  image->size=6;image->width=640;image->height=480;
  return 0;
}
int agora_control_scene_failed(const cJSON *local,
                               const struct agora_scene_request_s *request)
{
  unlocked();
  assert(cJSON_IsObject(local) && strlen(request->id)==32 && request->remaining_ms);
  assert(!atomic_load(&mock.scene_upload));
  atomic_fetch_add(&mock.scene_notify, 1);
  return 0;
}
int agora_control_scene_request(const cJSON *local,
                                const struct agora_scene_request_s *request,
                                const struct vision_badge_image_s *image)
{
  unlocked();
  assert(cJSON_IsObject(local) && request->id[0] && request->remaining_ms);
  assert(image->size == 6 && image->width == 640);
  if (g.camera_live) assert(!g.observing && !g.audio_quiesced);
  else assert(atomic_load(&mock.owners) == 0);
  assert(atomic_load(&mock.scene_capture) > atomic_load(&mock.scene_upload));
  atomic_fetch_add(&mock.scene_upload, 1);
  if (mock.http_burst)
    {
      uint64_t deadline=now_ms()+5000;
      while (!atomic_load(&mock.http_release) && !stopping(NULL) && now_ms()<deadline) usleep(1000);
      assert(atomic_load(&mock.http_release) || stopping(NULL));
    }
  return mock.scene_http_error;
}

int audio_service_capture_stream(audio_capture_cb_t sink, audio_cancel_cb_t cancel, void *arg)
{
  unlocked();
  assert(atomic_fetch_add(&mock.owners, 1) == 0);
  assert(!atomic_load(&mock.draining));
  atomic_fetch_add(&mock.capture, 1);
  int16_t pcm[TX_SAMPLES + 1];
  for (size_t i = 0; i < sizeof(pcm) / sizeof(pcm[0]); i++) pcm[i] = 321;
  int ret = 0;
  while (!cancel(arg) || (mock.capture_stuck && !stopping(NULL)))
    {
      ret = sink(arg, pcm, mock.tx_overflow ? TX_SAMPLES + 1 : PCM_SAMPLES / 2);
      assert(pcm[0] == 321 && pcm[PCM_SAMPLES / 2 - 1] == 321);
      if (ret) break;
      usleep(10000);
    }
  atomic_fetch_sub(&mock.owners, 1);
  return ret ? ret : -ECANCELED;
}
int audio_service_play_stream(audio_source_cb_t source, audio_cancel_cb_t cancel,
                              void *arg, unsigned int volume)
{
  unlocked(); assert(volume == (mock.expected_volume ? mock.expected_volume : 30));
  atomic_store(&mock.playback_volume, (int)volume);
  assert(atomic_fetch_add(&mock.owners, 1) == 0);
  atomic_fetch_add(&mock.play, 1);
  int16_t pcm[640];
  size_t startup_guard = PCM_SAMPLES;
  int ret = 0;
  while (!cancel(arg))
    {
      ret = source(arg, pcm, 640);
      if (ret == -EAGAIN) { usleep(1000); continue; }
      if (ret <= 0) break;
      size_t guard = startup_guard < (size_t)ret ? startup_guard : (size_t)ret;
      for (size_t i=0; i<guard; i++) assert(pcm[i] == 0);
      startup_guard -= guard;
      atomic_fetch_add(&mock.guard_samples, guard);
      for (int i = (int)guard; i < ret; i++)
        {
          assert((mock.burst_pattern && pcm[i] == 1000 +
                    ((atomic_load(&mock.played_samples) + i - (int)guard) / (int)PCM_SAMPLES) % 70) ||
                 (!mock.burst_pattern && pcm[i] == 123) || (mock.zero_pcm && (pcm[i] >= -3 && pcm[i] <= 3)) ||
                 (mock.codec_silence && pcm[i] >= -4 && pcm[i] <= 4) ||
                 (mock.tiny_pcm && (pcm[i] == 5 || pcm[i] == -5)));
          if (mock.tiny_pcm && (pcm[i] == 5 || pcm[i] == -5))
            {
              assert(pcm[i] == (i % 2 ? -5 : 5));
              atomic_fetch_add(&mock.tiny_samples, 1);
            }
        }
      atomic_fetch_add(&mock.played_samples, ret - guard);
      if (mock.burst_pattern) usleep((unsigned int)ret * 1000000u / 16000u);
    }
  if (!ret && !cancel(arg))
    {
      atomic_store(&mock.draining, true);
      /* COMPLETE is later than queue-empty. Capture must not resume here. */
      usleep(40000);
      atomic_fetch_add(&mock.complete, 1);
      atomic_store(&mock.draining, false);
    }
  atomic_fetch_sub(&mock.owners, 1);
  return cancel(arg) ? -ECANCELED : ret;
}

static int command(const char *action)
{
  char text[512];
  int ret = agora_service_command(action, text, sizeof(text));
  assert(!strstr(text, "01234567890123456789012345678901"));
  return ret;
}
static void write_config(const char *json)
{
  FILE *file = fopen(AGORA_CONFIG, "w"); assert(file);
  assert(fputs(json, file) >= 0); assert(fclose(file) == 0);
}
static void wait_idle(void)
{
  uint64_t deadline = now_ms() + 10000;
  while (now_ms() < deadline)
    {
      nxmutex_lock(&g.lock); bool running = g.running; nxmutex_unlock(&g.lock);
      if (!running) return;
      usleep(1000);
    }
  assert(!"session failed to stop");
}
static void wait_active(void)
{
  /* Host lifecycle watchdog, not the production join/receive deadline.
   * Leave headroom for this mechanical-disk VM's scheduling stalls. */
  uint64_t deadline = now_ms() + 10000;
  while (now_ms() < deadline)
    {
      nxmutex_lock(&g.lock); bool active = g.phase == ACTIVE; nxmutex_unlock(&g.lock);
      if (active) return;
      usleep(1000);
    }
  /* The VM can be descheduled across the final polling deadline. Inspect
   * the completed state once more, as wait_count already does; this test
   * checks mock lifecycle ordering, not real-time scheduling latency. */
  nxmutex_lock(&g.lock);
  bool active = g.phase == ACTIVE;
  if (!active) fprintf(stderr, "mock join wait: phase=%d running=%d error=%d sdk=%d audio=%d\n",
                       g.phase, g.running, g.error, g.sdk_error, g.audio_error);
  nxmutex_unlock(&g.lock);
  assert(active && "join callback was not observed");
}
static void wait_count(atomic_int *counter, int wanted)
{
  uint64_t deadline = now_ms() + 10000;
  while (now_ms() < deadline && atomic_load(counter) < wanted) usleep(1000);
  if (atomic_load(counter) < wanted)
    fprintf(stderr, "mock counter wait: wanted=%d actual=%d capture=%d play=%d complete=%d send=%d\n",
            wanted, atomic_load(counter), atomic_load(&mock.capture), atomic_load(&mock.play),
            atomic_load(&mock.complete), atomic_load(&mock.send));
  assert(atomic_load(counter) >= wanted);
}
static void emit_pcm(size_t n, int type)
{
  int16_t *pcm = malloc(n * sizeof(*pcm)); assert(pcm);
  for (size_t i = 0; i < n; i++) pcm[i] = 123;
  audio_frame_info_t info = { .data_type = type };
  mock.events->on_audio_data(42, 456, 1, pcm, n * sizeof(*pcm), &info);
  free(pcm);
}
static void emit_ordered_burst(void)
{
  int16_t pcm[PCM_SAMPLES];
  audio_frame_info_t info = { .data_type = AUDIO_DATA_TYPE_PCM };
  for (int frame=0; frame<70; frame++)
    {
      for (size_t i=0; i<PCM_SAMPLES; i++) pcm[i] = (int16_t)(1000 + frame);
      mock.events->on_audio_data(42,456,(uint16_t)frame,pcm,sizeof(pcm),&info);
    }
}
static void wait_vision_count(unsigned int completed, unsigned int failed)
{
  uint64_t deadline=now_ms()+10000;
  for (;;)
    {
      nxmutex_lock(&g.lock);
      bool ready=g.vision_completed>=completed && g.vision_failed>=failed;
      nxmutex_unlock(&g.lock);
      if (ready) return;
      assert(now_ms()<deadline && "vision result not published");
      usleep(1000);
    }
}
static void assert_clean(void)
{
  assert(!g.running && !g.blocked && !g.initialized && !g.connection &&
         !g.audio_started && !g.lease_started && !g.camera_paused && !g.camera_live_owned);
  assert(atomic_load(&mock.camera_pause) + atomic_load(&mock.camera_borrow) ==
         atomic_load(&mock.camera_resume));
  assert(!g.local && !g.session && !g.tx.data && !g.rx.data && !g.config.token[0]);
  assert(atomic_load(&mock.allocations) == 0 && atomic_load(&mock.owners) == 0);
  assert(atomic_load(&mock.scene_allocations) == 0 && !mock.scene_buffer);
  assert(atomic_load(&mock.rx_allocations) == 0 && !mock.rx_buffer);
  assert(atomic_load(&mock.sdk_rx_attempts) == 0);
}
static void invalid_field(const char *name, const char *replacement)
{
  cJSON *root = cJSON_Parse(valid), *value = cJSON_Parse(replacement);
  assert(root && value && cJSON_ReplaceItemInObjectCaseSensitive(root, name, value));
  char *json = cJSON_PrintUnformatted(root); assert(json);
  write_config(json); free(json); cJSON_Delete(root);
  int calls = atomic_load(&mock.init);
  assert(command("start") == 0); wait_idle(); assert_clean();
  assert(g.error == -EINVAL && atomic_load(&mock.init) == calls);
}
#ifdef R1_WAKE_INTEGRATION
static int wake_stack(pthread_attr_t *attr,size_t bytes) {assert(attr && bytes==8192);return 0;}
#define pthread_attr_setstacksize wake_stack
#define pthread_attr_setinheritsched mock_attr_inherit
#define pthread_attr_setschedpolicy mock_attr_policy
#define pthread_attr_setschedparam mock_attr_priority
#include "../../app/vision_badge/src/wake_service.c"
#undef pthread_attr_setstacksize
#undef pthread_attr_setinheritsched
#undef pthread_attr_setschedpolicy
#undef pthread_attr_setschedparam
static pthread_t wake_task;
static int (*wake_entry)(int,char**);
static void *wake_bridge(void *arg) {(void)arg;wake_entry(0,NULL);return NULL;}
int kthread_create(const char *name,int priority,int stack,int (*entry)(int,char**),char *const args[])
{assert(!strcmp(name,"r1-wake") && priority>0 && priority<99 && stack==8192 && !args);wake_entry=entry;assert(!pthread_create(&wake_task,NULL,wake_bridge,NULL));return 123;}
int nxsig_usleep(unsigned int us) {usleep(us);return 0;}
struct r1_wake_model_s {int unused;};
const unsigned char r1_wake_model_data[16]={0};const size_t r1_wake_model_bytes=16;
static unsigned wake_model_frames;
int r1_wake_model_open(const unsigned char *data,size_t size,bool release,struct r1_wake_model_s **out)
{assert(data && size==16);(void)release;*out=bk7258_psram_malloc(sizeof(**out));return *out?0:-ENOMEM;}
void r1_wake_model_close(struct r1_wake_model_s *model) {bk7258_psram_free(model);}
void r1_wake_model_gap(struct r1_wake_model_s *model) {assert(model);}
size_t r1_wake_model_arena_used(const struct r1_wake_model_s *model) {assert(model);return 100;}
int r1_wake_model_selftest(int q[3]) {q[0]=q[1]=0;q[2]=127;return 0;}
int r1_wake_model_frontend_selftest(unsigned int *changed,unsigned int *max_delta){*changed=*max_delta=0;return 0;}
int r1_wake_model_set_threshold(struct r1_wake_model_s *model,unsigned int p){assert(model && p>=50 && p<=95);return 0;}
int r1_wake_model_diagnostics(const struct r1_wake_model_s *model,struct r1_wake_model_diag_s *d)
{assert(model && d);memset(d,0,sizeof(*d));d->valid=true;d->scores_milli[2]=900;return 0;}
int r1_wake_model_feed(struct r1_wake_model_s *model,const int16_t pcm[320],uint64_t ms,float *score,bool *inferred)
{assert(model && pcm && ms);*score=.9f;*inferred=true;return ++wake_model_frames==3?1:0;}
#endif

int main(int argc, char **argv)
{
  if (argc > 1 && !strncmp(argv[1], "control-", 8))
    {
      bool busy = !strcmp(argv[1], "control-busy");
      bool disconnected = !strcmp(argv[1], "control-disconnected");
      bool transient = !strcmp(argv[1], "control-transient") || busy || disconnected;
      bool permanent = !strcmp(argv[1], "control-auth-fail");
      bool exhausted = !strcmp(argv[1], "control-retry-limit");
      assert(transient || permanent || exhausted);
      mock.vision_mode = true;
      write_config("{\"server_host\":\"127.0.0.1\",\"server_port\":8766,\"device_key\":\"mock-key\",\"volume\":30}");
      if (transient)
        {
          int error = busy ? -EBUSY : disconnected ? -ENOTCONN : -ECONNABORTED;
          atomic_store(&mock.keepalive_once, error);
          atomic_store(&mock.scene_number, 1);
          assert(command("start") == 0); wait_active();
          wait_count(&mock.scene_upload, 1);
          assert(g.phase == ACTIVE && !atomic_load(&mock.cloud_stop));
          assert(g.control_failures == 1 && g.control_recovered == 1 && !g.control_consecutive);
          assert(g.error == 0 && g.control_error == error);
          assert(atomic_load(&mock.sdk_scene_attempts) == 0);
          char stats[256];
          assert(agora_service_command("controlstats", stats, sizeof(stats)) == 0);
          assert(strstr(stats, "failures=1 consecutive=0 recovered=1") && strlen(stats) < sizeof(stats)-1);
          assert(atomic_load(&mock.cloud_start) == 1);
          if (disconnected) mock.events->on_connection_lost(42);
          else assert(command("stop") == 0);
          wait_idle(); assert_clean();
          if (disconnected) assert(g.error == -ENETDOWN);
          assert(command("init-test") == 0); wait_idle(); assert_clean();
          assert(!g.control_polls && !g.control_failures && !g.control_recovered);
        }
      else
        {
          mock.keepalive_error = permanent ? -EACCES : -ECONNABORTED;
          assert(command("start") == 0); wait_idle(); assert_clean();
          assert(atomic_load(&mock.keepalive) == (permanent ? 1 : 3));
          assert(g.control_consecutive == (permanent ? 1u : 3u));
          assert(g.error == mock.keepalive_error && atomic_load(&mock.cloud_start) == 1);
        }
      assert(atomic_load(&mock.cloud_stop) == 1);
      assert(unlink(AGORA_CONFIG) == 0);
      puts("CONTROL_RECOVERY_PASS single lost poll preserves RTC/scene; auth/3 failures stop; no new cloud join (transport mocked)");
      return 0;
    }
#ifdef R1_WAKE_INTEGRATION
  if (argc>1 && !strcmp(argv[1],"wake-scene-integration"))
    {
      cJSON *root=cJSON_Parse(valid);assert(root);
      assert(cJSON_AddStringToObject(root,"server_host","192.168.110.25"));
      char *config=cJSON_PrintUnformatted(root);assert(config);write_config(config);free(config);cJSON_Delete(root);
      mock.vision_mode=true;
      assert(command("wake-start")==0);wait_active();wait_count(&mock.capture,2);
      assert(w.triggers==1 && atomic_load(&mock.cloud_start)==1 && atomic_load(&mock.init)==1 && atomic_load(&mock.join)==1);
      atomic_store(&mock.scene_number,1);wait_count(&mock.scene_upload,1);
      emit_pcm(640,AUDIO_DATA_TYPE_PCM);wait_count(&mock.complete,1);wait_count(&mock.capture,3);
      assert(g.vision_completed==1 && !g.observing && !g_wake_busy);
      assert(command("wake-stop")==0 && command("stop")==0);wait_idle();assert_clean();
      pthread_cancel(wake_task);pthread_join(wake_task,NULL);
      assert(atomic_load(&mock.cloud_start)==1 && atomic_load(&mock.cloud_stop)==1);
      assert(unlink(AGORA_CONFIG)==0);
      puts("WAKE_TO_SCENE_INTEGRATION_PASS actual wake+Agora coordinators: MIC released before voice start, same lease photograph and reply, capture resumes; model scores/devices/RTC mocked");
      return 0;
    }
#endif
  if (argc>1 && !strcmp(argv[1],"wake-admission"))
    {
      write_config(valid);
      assert(command("wake-status")==0 && wake_delegations==1);
      assert(agora_service_wake_acquire()==0);
      assert(agora_service_wake_acquire()==-EBUSY);
      assert(agora_service_power_quiesce()==-EAGAIN && agora_service_wake_cancelled());
      assert(agora_service_power_resume()==-EBUSY);
      agora_service_wake_release(0);
      assert(agora_service_power_quiesce()==0 && agora_service_power_resume()==0);
      assert(agora_service_wake_acquire()==0);
      assert(command("start")==0);usleep(20000);
      assert(agora_service_wake_cancelled() && !atomic_load(&mock.init) && !atomic_load(&mock.camera_pause));
      agora_service_wake_release(0);wait_active();
      assert(agora_service_wake_acquire()==-EIO || g.running);
      assert(command("stop")==0);wait_idle();assert_clean();
      assert(agora_service_wake_acquire()==0);
      agora_service_wake_release(-EIO);
      assert(command("start")==-EIO && agora_service_power_quiesce()==-EIO);
      cleanup();assert(g.blocked && g_wake_fault);
      assert(command("wake-status")==0 && wake_delegations==2);
      assert(unlink(AGORA_CONFIG)==0);
      puts("AGORA_WAKE_ADMISSION_PASS actual voice/power admission waits for wake owner; cleanup cannot clear retained wake fault; SDK/Audio mocked");
      return 0;
    }
  if (argc > 1 && !strcmp(argv[1], "power-drain"))
    {
      write_config(valid);
      assert(command("start") == 0); wait_active();
      assert(agora_service_power_quiesce() == -EAGAIN);
      assert(command("start") == -ESHUTDOWN);
      wait_idle(); assert_clean();
      assert(agora_service_power_quiesce() == 0);
      assert(command("start") == -ESHUTDOWN);
      assert(agora_service_power_resume() == 0);
      assert(command("start") == 0); wait_active();
      assert(command("stop") == 0); wait_idle(); assert_clean();
      g.blocked = true;
      assert(agora_service_power_quiesce() == -EIO);
      assert(agora_service_power_resume() == -EIO);
      assert(command("start") == -ESHUTDOWN);
      g.blocked = false;
      assert(agora_service_power_resume() == 0);
      assert(unlink(AGORA_CONFIG) == 0);
      puts("AGORA_POWER_DRAIN_PASS real coordinator admission/cleanup/retained error; SDK/Audio mocked");
      return 0;
    }
  if (argc > 1 && !strncmp(argv[1], "live-camera", 11))
    {
      bool stop=!strcmp(argv[1],"live-camera-stop");
      bool expire=!strcmp(argv[1],"live-camera-expire");
      bool failed=!strcmp(argv[1],"live-camera-fail");
      bool feedback_expire=!strcmp(argv[1],"live-camera-feedback-expire");
      assert(stop || expire || failed || feedback_expire || !strcmp(argv[1],"live-camera"));
      assert(command("camera-live-on")==0 && g_camera_live_requested);
      mock.vision_mode=true;mock.http_burst=!stop && !expire && !failed && !feedback_expire;
      mock.scene_camera_error=failed ? -ETIMEDOUT : 0;
      mock.scene_remaining_ms=expire ? 50 : feedback_expire ? 200 : 5000;
      feedback_delay_us=feedback_expire ? 400000 : 0;
      cJSON *root=cJSON_Parse(valid);assert(root);
      assert(cJSON_AddStringToObject(root,"server_host","192.168.110.25"));
      char *config=cJSON_PrintUnformatted(root);assert(config);
      write_config(config);free(config);cJSON_Delete(root);
      assert(command("start")==0);wait_active();wait_count(&mock.capture,1);
      assert(g.camera_live_owned && !g.camera_paused && !g.observing);
      assert(command("camera-live-off")==-EBUSY && g_camera_live_requested);
      for(int number=1;number<=(stop || expire || failed || feedback_expire ? 1 : 3);number++)
        {
          atomic_store(&mock.live_release,false);atomic_store(&mock.http_release,false);
          atomic_store(&mock.scene_number,number);wait_count(&mock.scene_capture,number);
          if (stop) {assert(command("stop")==0);break;}
          if (feedback_expire)
            {
              atomic_store(&mock.live_release,true);wait_vision_count(0,1);
              assert(g.vision_error==-ETIMEDOUT && g.running && !g.stop);
              assert(!atomic_load(&mock.scene_upload) && !atomic_load(&mock.scene_notify));
              break;
            }
          if (failed)
            {
              atomic_store(&mock.live_release,true);
              wait_count(&mock.scene_notify,1);wait_vision_count(0,1);
              assert(g.vision_error==-ETIMEDOUT && g.running && !g.stop && !atomic_load(&mock.scene_upload));
              int sent=atomic_load(&mock.send);wait_count(&mock.send,sent+3);
              usleep(100000);
              assert(atomic_load(&mock.scene_notify)==1 && atomic_load(&mock.scene_capture)==1);
              break;
            }
          if (expire)
            {
              wait_vision_count(0,1);
              nxmutex_lock(&g.lock);
              assert(g.vision_failed==1 && g.vision_error==-ETIMEDOUT && g.running && !g.stop);
              nxmutex_unlock(&g.lock);
              assert(!atomic_load(&mock.scene_upload));break;
            }
          int sent=atomic_load(&mock.send);wait_count(&mock.send,sent+3);
          emit_pcm(640,AUDIO_DATA_TYPE_PCM);wait_count(&mock.complete,number*2-1);
          assert(atomic_load(&mock.scene_upload)==number-1 && !g.observing && !g.audio_quiesced);
          atomic_store(&mock.live_release,true);wait_count(&mock.scene_upload,number);
          sent=atomic_load(&mock.send);wait_count(&mock.send,sent+3);
          emit_pcm(640,AUDIO_DATA_TYPE_PCM);wait_count(&mock.complete,number*2);
          assert(g.vision_completed==(unsigned int)(number-1));
          atomic_store(&mock.http_release,true);
          wait_vision_count((unsigned int)number,0);
          assert(g.vision_completed==(unsigned int)number && !g.vision_error);
        }
      if (!stop) assert(command("stop")==0);
      wait_idle();assert_clean();
      assert(atomic_load(&mock.camera_borrow)==1 && !atomic_load(&mock.camera_pause));
      assert(atomic_load(&mock.cloud_start)==1 && atomic_load(&mock.cloud_stop)==1);
      assert(atomic_load(&mock.init)==1 && atomic_load(&mock.join)==1);
      assert(!g.vision_flushed && !g.error);
      assert(command("camera-live-off")==0 && !g_camera_live_requested);
      assert(unlink(AGORA_CONFIG)==0);
      puts("AGORA_LIVE_CAMERA_PASS same lease, Audio progresses during fresh snapshot and upload; SDK/devices mocked");
      return 0;
    }
  if (argc > 1 && !strcmp(argv[1], "early-join-greeting"))
    {
      assert(command("rxcache-test")==0);
      mock.rx_alloc_fail=true; assert(command("rxcache-test")==-ENOMEM); mock.rx_alloc_fail=false;
      mock.scene_alloc_fail=true; assert(command("rxcache-test")==-ENOMEM); mock.scene_alloc_fail=false;
      assert(!atomic_load(&mock.cloud_start) && !atomic_load(&mock.init)); assert_clean();
      mock.pre_join_audio=true;
      write_config(valid); assert(command("start")==0); wait_active();
      assert(command("rxcache-test")==-EBUSY);
      wait_count(&mock.complete,1);
      assert(atomic_load(&mock.played_samples)==PCM_SAMPLES);
      assert(atomic_load(&mock.guard_samples)==PCM_SAMPLES);
      assert(!g.rx_state_drop && !g.error);
      assert(command("stop")==0); wait_idle(); assert_clean();
      assert(unlink(AGORA_CONFIG)==0);
      puts("EARLY_JOIN_PCM_PASS: expected UID queued before join callback, exact PCM after startup guard; SDK mocked");
      return 0;
    }
  if (argc > 1 && !strcmp(argv[1], "vision-long-poll-stop"))
    {
      mock.vision_mode = mock.long_poll = true;
      cJSON *root = cJSON_Parse(valid); assert(root);
      assert(cJSON_AddStringToObject(root, "server_host", "192.168.110.25"));
      char *config = cJSON_PrintUnformatted(root); assert(config);
      write_config(config); free(config); cJSON_Delete(root);
      assert(command("start") == 0); wait_active(); wait_count(&mock.keepalive, 1);
      assert(g.vision_enabled && g.vision_long_poll);
      uint64_t started = now_ms();
      assert(command("stop") == 0); wait_idle(); assert_clean();
      assert(now_ms() - started < 2000 && !g.error);
      assert(atomic_load(&mock.cloud_start) == 1 && atomic_load(&mock.cloud_stop) == 1);
      assert(!atomic_load(&mock.scene_capture) && !atomic_load(&mock.scene_upload));
      assert(unlink(AGORA_CONFIG) == 0);
      puts("VISION_LONG_POLL_STOP_PASS: cancel lease before join; no Audio/Camera/cloud transport");
      return 0;
    }
  if (argc > 1 && (!strcmp(argv[1], "vision-rx-alloc-fail") ||
                   !strcmp(argv[1], "vision-rx-limit")))
    {
      mock.vision_mode = true;
      mock.rx_alloc_fail = !strcmp(argv[1], "vision-rx-alloc-fail");
      cJSON *root = cJSON_Parse(valid); assert(root);
      assert(cJSON_AddStringToObject(root,"server_host","192.168.110.25"));
      char *config = cJSON_PrintUnformatted(root); assert(config);
      write_config(config); free(config); cJSON_Delete(root);
      assert(command("start") == 0);
      if (!mock.rx_alloc_fail)
        { wait_active(); emit_pcm(RX_VISION_SAMPLES+1,AUDIO_DATA_TYPE_PCM); }
      wait_idle(); assert_clean();
      assert(g.error == (mock.rx_alloc_fail ? -ENOMEM : -ENOBUFS));
      assert(atomic_load(&mock.cloud_start)==1 && atomic_load(&mock.cloud_stop)==1);
      assert(!atomic_load(&mock.scene_upload));
      if (mock.rx_alloc_fail) assert(!atomic_load(&mock.init));
      assert(unlink(AGORA_CONFIG)==0);
      puts("VISION_RX_BOUND_PASS: bounded system-heap cache, allocation/oversize failure cleanup; SDK mocked");
      return 0;
    }
  if (argc > 1 && !strncmp(argv[1], "vision-", 7))
    {
      bool multiple = !strcmp(argv[1],"vision-multiple") || !strcmp(argv[1],"vision-burst-multiple");
      bool burst = !strcmp(argv[1], "vision-burst") || !strcmp(argv[1],"vision-burst-multiple") ||
                   !strcmp(argv[1],"vision-http-burst");
      bool handoff = !strcmp(argv[1], "vision-handoff") || multiple || burst;
      bool stop = !strcmp(argv[1], "vision-stop");
      bool timeout = !strcmp(argv[1], "vision-timeout");
      bool quiesce = !strcmp(argv[1], "vision-quiesce-fail");
      bool camera = !strcmp(argv[1], "vision-camera-fail");
      bool http = !strcmp(argv[1], "vision-http-fail");
      bool alloc = !strcmp(argv[1], "vision-alloc-fail");
      assert(handoff || stop || timeout || quiesce || camera || http || alloc);
      mock.vision_mode = true;
      mock.burst_pattern = burst;
      mock.http_burst = !strcmp(argv[1],"vision-http-burst");
      mock.scene_camera_error = camera ? -EIO : 0;
      mock.scene_http_error = http ? -EREMOTEIO : 0;
      mock.scene_alloc_fail = alloc;
      mock.capture_stuck = quiesce;
      mock.scene_delay_us = burst ? 500000 : (handoff || stop || timeout) ? 100000 : 0;
      mock.scene_remaining_ms = (timeout || quiesce) ? 50 : burst ? 5000 : 2000;
      cJSON *root = cJSON_Parse(valid);assert(root);
      assert(cJSON_AddStringToObject(root, "server_host", "192.168.110.25"));
      char *config = cJSON_PrintUnformatted(root);assert(config);
      write_config(config);free(config);cJSON_Delete(root);
      assert(command("start") == 0);wait_active();wait_count(&mock.capture, 1);
      atomic_store(&mock.scene_number, 1);
      if (stop || handoff)
        {
          wait_count(&mock.scene_capture, 1);
          if (mock.http_burst) wait_count(&mock.scene_upload,1);
          if (stop) assert(command("stop") == 0);
          else if (burst)
            {
              /*1.4s of late decoded PCM while the camera owns Audio's pause.
               * SDK callback cannot wait for a consumer that is quiesced. */
              emit_ordered_burst();
              assert(g.buffer_error == 0);
              if (mock.http_burst) atomic_store(&mock.http_release,true);
            }
          else emit_pcm(640, AUDIO_DATA_TYPE_PCM); /* Queue reply during capture. */
        }
      uint64_t end = now_ms() + 10000;
      while (now_ms() < end)
        {
          nxmutex_lock(&g.lock);
          bool done = g.vision_completed || g.vision_failed;
          nxmutex_unlock(&g.lock);
          if (done) break;
          usleep(1000);
        }
      nxmutex_lock(&g.lock);
      assert(g.vision_requests == 1 && !g.observing);
      assert(g.vision_completed == (unsigned int)handoff);
      assert(g.vision_failed == (unsigned int)!handoff);
      int scene_error = g.vision_error;
      nxmutex_unlock(&g.lock);
      if (handoff)
        {
          assert(atomic_load(&mock.sdk_scene_attempts) == 0);
          assert(!scene_error);wait_count(&mock.scene_upload, 1);
          wait_count(&mock.complete, 1);wait_count(&mock.capture, 2);
          if (burst) assert(atomic_load(&mock.played_samples) == 70 * (int)PCM_SAMPLES);
          int pings = atomic_load(&mock.keepalive);wait_count(&mock.keepalive, pings + 2);
          assert(atomic_load(&mock.scene_capture) == 1 && atomic_load(&mock.scene_upload) == 1);
          if (multiple)
            for (int request=2;request<=3;request++)
              {
                atomic_store(&mock.scene_number,request);
                wait_count(&mock.scene_capture,request);
                if (burst) emit_ordered_burst(); else emit_pcm(640,AUDIO_DATA_TYPE_PCM);
                wait_count(&mock.scene_upload,request);
                wait_count(&mock.complete,request);wait_count(&mock.capture,request+1);
                int seen=atomic_load(&mock.keepalive);wait_count(&mock.keepalive,seen+2);
                assert(atomic_load(&mock.scene_capture)==request && atomic_load(&mock.scene_upload)==request);
                assert(atomic_load(&mock.init)==1 && atomic_load(&mock.join)==1);
                if (burst) assert(atomic_load(&mock.played_samples)==request*70*(int)PCM_SAMPLES);
              }
        }
      else if (stop) assert(scene_error == -ECANCELED && !atomic_load(&mock.scene_upload));
      else if (timeout || quiesce) assert(scene_error == -ETIMEDOUT && !atomic_load(&mock.scene_upload));
      else if (camera) assert(scene_error == -EIO && !atomic_load(&mock.scene_upload) && atomic_load(&mock.scene_notify)==1);
      else if (alloc) assert(scene_error == -ENOMEM && !atomic_load(&mock.scene_capture));
      else assert(scene_error == -EREMOTEIO);
      if (!stop && !camera && !quiesce) assert(command("stop") == 0);
      wait_idle();assert_clean();
      assert(atomic_load(&last_activity) == (stop ? VISION_BADGE_STAGE_IDLE :
             handoff ? VISION_BADGE_STAGE_DONE : VISION_BADGE_STAGE_ERROR));
      if (handoff || http)
        assert(atomic_load(&activity_seen) & (1u << VISION_BADGE_STAGE_QUERY));
      if (handoff || stop || camera || http)
        assert(atomic_load(&activity_seen) & (1u << VISION_BADGE_STAGE_CAPTURE));
      assert(g.error == (camera ? -EIO : quiesce ? -ETIMEDOUT : 0));
      assert(atomic_load(&mock.cloud_start) == 1 && atomic_load(&mock.cloud_stop) == 1);
      assert(unlink(AGORA_CONFIG) == 0);
      puts("Agora vision handoff PASS: pause ownership/no concurrent Audio, bounded stop/failure, same RTC lease; hardware mocked");
      return 0;
    }
  if (argc > 1 && !strcmp(argv[1], "mic-gain"))
    {
      char text[256];
      assert(command("mic-gain") == 0 && g.mic_gain == 16);
      const char *invalid[] = {"mic-gain 0", "mic-gain 17", "mic-gain -1",
        "mic-gain +1", "mic-gain 1.5", "mic-gain 1 extra", "mic-gain ",
        "mic-gain 999999999999999999999999999999999999"};
      for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(command(invalid[i]) == -EINVAL && g.mic_gain == 16);
      g.blocked = true;
      assert(command("mic-gain 1") == -EIO && g.mic_gain == 16);
      g.blocked = false;
      int16_t pcm[] = {INT16_MIN, -2049, -2048, -313, -1, 0, 1, 313,
                       2047, 2048, INT16_MAX};
      int16_t original[sizeof(pcm) / sizeof(pcm[0])];
      memcpy(original, pcm, sizeof(pcm));
      uint64_t clipped = 0;
      assert(scale_tx_pcm(pcm, sizeof(pcm) / sizeof(pcm[0]), 1, &clipped) == 32768);
      assert(clipped == 0 && !memcmp(pcm, original, sizeof(pcm)));
      assert(scale_tx_pcm(pcm, sizeof(pcm) / sizeof(pcm[0]), 16, &clipped) == 32768);
      const int16_t expected[] = {INT16_MIN, INT16_MIN, INT16_MIN, -5008, -16,
                                 0, 16, 5008, 32752, INT16_MAX, INT16_MAX};
      assert(clipped == 4 && !memcmp(pcm, expected, sizeof(pcm)));
      int16_t silence[PCM_SAMPLES] = {0};
      assert(scale_tx_pcm(silence, PCM_SAMPLES, 16, &clipped) == 0 && clipped == 4);
      /* Whole coordinator: default gain, idle rollback, and next-session
       * persistence. Existing mocks verify raw321 and downlink123 unchanged. */
      write_config(valid);
      assert(command("start") == 0); wait_active(); wait_count(&mock.send, 2);
      assert(command("mic-gain 1") == -EBUSY && g.mic_gain == 16);
      assert(agora_service_command("micstats", text, sizeof(text)) == 0);
      assert(strstr(text, "raw_peak=321 raw_mean_max=321 gain=16 prepared_peak=5136 clipped=0"));
      assert(strlen(text) < sizeof(text) - 1);
      assert(command("stop") == 0); wait_idle(); assert_clean();
      assert(g.mic_tx_peak == 5136 && g.mic_clipped == 0);
      assert(command("mic-gain 1") == 0); mock.expected_gain = 1;
      int sends = atomic_load(&mock.send);
      assert(command("start") == 0); wait_active(); wait_count(&mock.send, sends + 2);
      assert(command("stop") == 0); wait_idle(); assert_clean();
      assert(g.mic_tx_peak == 321 && g.mic_gain == 1 && !g.mic_clipped);
      assert(command("init-test") == 0); wait_idle(); assert_clean();
      assert(!g.mic_peak && !g.mic_tx_peak && !g.mic_clipped && g.mic_gain == 1);
      assert(unlink(AGORA_CONFIG) == 0);
      puts("Agora microphone gain PASS: signed saturation/zero/1x rollback, real coordinator default16x, raw/downlink preserved, counters/reset; SDK/audio mocked");
      return 0;
    }
  if (argc == 3 && !strcmp(argv[1], "g722-conversation"))
    {
      /* Real-time PCM cadence and production tail timeout. SDK, DMA and
       * microphone remain mocks: this verifies turn ownership, not hearing. */
      assert(AGORA_RX_IDLE_MS == 350u);
      FILE *file = fopen(argv[2], "rb"); assert(file);
      mock.codec_silence = true;
      write_config(valid);
      assert(command("start") == 0); wait_active();
      wait_count(&mock.capture, 1);
      wait_count(&mock.send, 2);
      audio_frame_info_t info = {.data_type = AUDIO_DATA_TYPE_PCM};
      int16_t pcm[PCM_SAMPLES];
      for (int turn = 0; turn < 3; turn++)
        {
          int captures = atomic_load(&mock.capture);
          int plays = atomic_load(&mock.play);
          int completes = atomic_load(&mock.complete);
          for (int frame = 0; frame < 10; frame++)
            {
              emit_pcm(PCM_SAMPLES, AUDIO_DATA_TYPE_PCM);
              usleep(20000);
            }
          rewind(file);
          unsigned int frames = 0;
          size_t count;
          while ((count = fread(pcm, sizeof(pcm[0]), PCM_SAMPLES, file)) != 0)
            {
              assert(count == PCM_SAMPLES);
              for (size_t i = 0; i < count; i++) assert(pcm[i] >= -4 && pcm[i] <= 4);
              mock.events->on_audio_data(42, 456, 1, pcm, sizeof(pcm), &info);
              frames++;
              usleep(20000);
            }
          assert(!ferror(file) && frames == 200);
          assert(atomic_load(&mock.play) == plays + 1);
          assert(atomic_load(&mock.complete) == completes + 1);
          assert(atomic_load(&mock.capture) == captures + 1);
          nxmutex_lock(&g.lock);
          assert(!g.playing && !g.rx.count && !g.error);
          nxmutex_unlock(&g.lock);
          /* Receiving quiet callbacks must not merely reopen MIC: actual
           * outgoing frames must resume before the next reply. */
          int sends = atomic_load(&mock.send);
          wait_count(&mock.send, sends + 3);
        }
      /* New speech arrives after EOF but before hardware COMPLETE. It must
       * remain queued for the next playback, with no intervening MIC owner. */
      int captures = atomic_load(&mock.capture);
      int completes = atomic_load(&mock.complete);
      int samples = atomic_load(&mock.played_samples);
      emit_pcm(PCM_SAMPLES, AUDIO_DATA_TYPE_PCM);
      uint64_t deadline = now_ms() + 10000;
      while (!atomic_load(&mock.draining) && now_ms() < deadline) usleep(1000);
      assert(atomic_load(&mock.draining));
      emit_pcm(PCM_SAMPLES, AUDIO_DATA_TYPE_PCM);
      wait_count(&mock.complete, completes + 2);
      wait_count(&mock.capture, captures + 1);
      assert(atomic_load(&mock.capture) == captures + 1);
      assert(atomic_load(&mock.played_samples) == samples + 2 * (int)PCM_SAMPLES);
      assert(command("stop") == 0); wait_idle(); assert_clean();
      assert(!g.error && !g.buffer_error && !g.rx_overflows && !g.tx_overflows);
      assert(fclose(file) == 0 && unlink(AGORA_CONFIG) == 0);
      puts("Agora 350ms conversation PASS: three G722-tail turns resume TX; speech during COMPLETE preserved; SDK/audio mocked");
      return 0;
    }
  if (argc == 3 && !strcmp(argv[1], "g722-silence-file"))
    {
      /* Real FFmpeg G722 round-trip of digital silence, not a constant
       * hand-written +/-3 fixture. The pinned BK decoder remains unmodified
       * and is not claimed to be exercised by this host test. */
      FILE *file = fopen(argv[2], "rb"); assert(file);
      mock.codec_silence = true;
      write_config(valid);
      assert(command("start") == 0); wait_active();
      wait_count(&mock.capture, 1);
      int captures = atomic_load(&mock.capture);
      emit_pcm(PCM_SAMPLES, AUDIO_DATA_TYPE_PCM);
      wait_count(&mock.play, 1);
      int16_t pcm[PCM_SAMPLES];
      unsigned int frames = 0, fours = 0;
      audio_frame_info_t info = {.data_type = AUDIO_DATA_TYPE_PCM};
      size_t count;
      while ((count = fread(pcm, sizeof(pcm[0]), PCM_SAMPLES, file)) != 0)
        {
          assert(count == PCM_SAMPLES);
          for (size_t i = 0; i < count; i++)
            {
              assert(pcm[i] >= -4 && pcm[i] <= 4);
              if (pcm[i] == 4 || pcm[i] == -4) fours++;
            }
          mock.events->on_audio_data(42, 456, 1, pcm, sizeof(pcm), &info);
          frames++;
          usleep(5000);
        }
      assert(!ferror(file) && fclose(file) == 0 && frames == 200 && fours > 0);
      /* Check while callbacks have only just stopped: no artificial idle
       * gap may rescue an implementation that restarts on decoded noise. */
      assert(atomic_load(&mock.capture) > captures);
      assert(atomic_load(&mock.play) == 1 && atomic_load(&mock.complete) == 1);
      nxmutex_lock(&g.lock);
      assert(!g.playing && !g.rx.count && g.rx_quiet_frames == frames);
      nxmutex_unlock(&g.lock);
      assert(command("stop") == 0); wait_idle(); assert_clean();
      assert(!g.error && !g.buffer_error && !g.rx_overflows && !g.tx_overflows);
      assert(unlink(AGORA_CONFIG) == 0);
      printf("Agora real G722 silence PASS: %u frames, %u samples at +/-4, one playback, capture resumed; BK SDK/audio mocked\n", frames, fours);
      return 0;
    }
  if (argc > 1 && (!strcmp(argv[1], "continuous-silence") ||
                  !strcmp(argv[1], "continuous-noise")))
    {
      mock.zero_pcm = true;
      write_config(valid);
      assert(command("start") == 0); wait_active();
      wait_count(&mock.capture, 1);
      int captures = atomic_load(&mock.capture);
      emit_pcm(PCM_SAMPLES, AUDIO_DATA_TYPE_PCM);
      wait_count(&mock.play, 1);
      int16_t zero[PCM_SAMPLES] = {0};
      bool residual = !strcmp(argv[1], "continuous-noise");
      if (residual)
        for (size_t i = 0; i < PCM_SAMPLES; i++) zero[i] = i % 2 ? -3 : 3;
      audio_frame_info_t info = {.data_type = AUDIO_DATA_TYPE_PCM};
      /* A decoder/jitter worker can keep returning silent PCM after speech.
       * There is deliberately no RX callback gap for the idle timer. */
      for (int i = 0; i < 80; i++)
        {
          mock.events->on_audio_data(42, 456, 1, zero, sizeof(zero), &info);
          usleep(5000);
        }
      assert(atomic_load(&mock.capture) > captures);
      assert(atomic_load(&mock.complete) == 1);
      assert(atomic_load(&mock.play) == 1);
      nxmutex_lock(&g.lock);
      assert(AGORA_RX_SILENCE_PEAK == 4 && g.rx_zero_frames == (residual ? 0 : 80));
      assert(g.rx_quiet_frames == 80 && g.rx_quiet_ignored > 0 && g.rx_peak == 123);
      assert(g.response_number==1 && g.response_rx_ms!=0);
      nxmutex_unlock(&g.lock);
      char text[256];
      assert(agora_service_command("rxstats", text, sizeof(text)) == 0);
      assert(strstr(text, "playing=0 queued=0") && strlen(text) < sizeof(text) - 1);
      assert(command("stop") == 0); wait_idle(); assert_clean();
      assert(!g.error && !g.buffer_error && !g.tx_overflows && !g.rx_overflows);
      mock.tiny_pcm = true;
      assert(command("start") == 0); wait_active();
      nxmutex_lock(&g.lock);
      assert(!g.rx_peak && !g.rx_zero_frames && !g.rx_quiet_frames && !g.rx_quiet_ignored && !g.last_rx);
      assert(g.response_number==0 && g.response_rx_ms==0);
      nxmutex_unlock(&g.lock);
      for (int i = 0; i < 12; i++)
        {
          mock.events->on_audio_data(42, 456, 1, zero, sizeof(zero), &info);
          usleep(5000);
        }
      nxmutex_lock(&g.lock);
      assert(atomic_load(&mock.play) == 1 && g.down == 0);
      assert(g.rx_zero_frames == (residual ? 0 : 12) && g.rx_quiet_ignored == 12);
      assert(g.response_number==0 && g.response_rx_ms==0);
      nxmutex_unlock(&g.lock);
      /* Preserve the first amplitude above the G722 +/-4 noise floor,
       * including an odd input address and exact alternating sign order. */
      unsigned char tiny[PCM_SAMPLES * 2 + 1];
      for (size_t i = 0; i < PCM_SAMPLES; i++)
        {
          int16_t value = i % 2 ? -5 : 5;
          memcpy(tiny + 1 + i * sizeof(value), &value, sizeof(value));
        }
      mock.events->on_audio_data(42, 456, 1, tiny + 1, sizeof(tiny) - 1, &info);
      wait_count(&mock.complete, 2);
      assert(atomic_load(&mock.tiny_samples) == PCM_SAMPLES);
      nxmutex_lock(&g.lock); assert(g.rx_peak == 5); nxmutex_unlock(&g.lock);
      assert(command("stop") == 0); wait_idle(); assert_clean();
      assert(unlink(AGORA_CONFIG) == 0);
      puts(residual ? "Agora continuous decoder residue PASS: +/-3 tail drains and capture resumes, +/-5 preserved; SDK/audio mocked" :
           "Agora continuous silence PASS: playback drains and capture resumes while silent PCM callbacks continue; SDK/audio mocked");
      return 0;
    }
  if (argc > 1 && !strcmp(argv[1], "continuous-downlink"))
    {
      write_config(valid); mock.early_audio = true;
      assert(command("start") == 0); wait_active();
      wait_count(&mock.complete, 1); wait_count(&mock.capture, 1);
      assert(atomic_load(&mock.played_samples) == 640);
      for (int turn = 0; turn < 12; turn++)
        {
          int captures = atomic_load(&mock.capture);
          int sends = atomic_load(&mock.send);
          wait_count(&mock.send, sends + 2);
          for (int frame = 0; frame < 8; frame++)
            { emit_pcm(PCM_SAMPLES, AUDIO_DATA_TYPE_PCM); usleep(10000); }
          wait_count(&mock.complete, turn + 2);
          wait_count(&mock.capture, captures + 1);
          assert(atomic_load(&mock.played_samples) == 640 + (turn + 1) * 8 * (int)PCM_SAMPLES);
        }
      assert(command("stop") == 0); wait_idle(); assert_clean();
      assert(!g.error && !g.buffer_error && !g.rx_overflows && !g.tx_overflows);
      assert(g.down == 640 + 12 * 8 * PCM_SAMPLES && g.rx_seen == 97 && !g.rx_state_drop);
      assert(command("start") == 0); wait_active();
      wait_count(&mock.complete, 14); wait_count(&mock.capture, 14);
      assert(command("stop") == 0); wait_idle(); assert_clean();
      assert(!g.error && g.down == 640 && g.rx_seen == 1);
      assert(unlink(AGORA_CONFIG) == 0);
      puts("Agora continuous downlink PASS: early greeting,12 turns of exact PCM delivery, capture resume, no queue loss, restart/reset; SDK/audio mocked");
      return 0;
    }
  if (argc > 1 && !strcmp(argv[1], "link-diagnostics"))
    {
      char text[256];
      write_config(valid);
      assert(command("start") == 0); wait_active();
      wait_count(&mock.send, 2);
      assert(net_resets == 1);
      mock.events->on_user_joined(42, 999, 0);
      mock.events->on_user_joined(42, 456, 0);
      int16_t pcm = 123;
      audio_frame_info_t info = {.data_type = AUDIO_DATA_TYPE_PCM};
      mock.events->on_audio_data(42, 999, 1, &pcm, sizeof(pcm), &info);
      assert(agora_service_command("link", text, sizeof(text)) == 0);
      assert(strstr(text, "remote_seen=2 expected_seen=1 rx_seen=1 uid_drop=1 state_drop=0"));
      assert(strstr(text, "mic_peak=321 mic_mean_max=321"));
      assert(strlen(text) < sizeof(text) - 1);
      assert(command("stop") == 0); wait_idle(); assert_clean();
      assert(g.remote_seen == 2 && g.expected_seen == 1 && g.rx_uid_drop == 1);
      assert(agora_service_command("netstats", text, sizeof(text)) == 0);
      assert(strstr(text, "mocked counters"));
      assert(command("init-test") == 0); wait_idle(); assert_clean();
      assert(net_resets == 2 && !g.remote_seen && !g.rx_seen && !g.mic_peak);
      nxmutex_lock(&g.lock);
      g.tx.capacity = 2; g.tx.count = g.tx.head = 0;
      g.tx.data = malloc(4); assert(g.tx.data);
      g.stop = g.playing = false;
      nxmutex_unlock(&g.lock);
      int16_t edge[2] = {INT16_MIN, INT16_MAX};
      assert(capture_pcm(NULL, edge, 2) == 0);
      assert(g.mic_peak == 32768 && g.mic_mean_max == 32767);
      free(g.tx.data); memset(&g.tx, 0, sizeof(g.tx));
      assert(unlink(AGORA_CONFIG) == 0);
      puts("Agora link diagnostics PASS: remote/UID filters, waveform range, retained counters/reset and256-byte response; SDK/audio mocked");
      return 0;
    }
  if (argc > 1 && !strcmp(argv[1], "sched-failure"))
    {
      write_config(valid);
      reject_priority = TX_PRIORITY;
      assert(command("start") == -EPERM); assert_clean();
      assert(!atomic_load(&mock.init) && !atomic_load(&mock.send));
      reject_priority = AUDIO_PRIORITY;
      assert(command("start") == 0); wait_idle(); assert_clean();
      assert(g.error == -EPERM && !atomic_load(&mock.capture));
      assert(unlink(AGORA_CONFIG) == 0);
      puts("Agora scheduling failure PASS: no fallback to inherited priority, clean startup failure; scheduler mocked");
      return 0;
    }
  if (argc > 1 && !strcmp(argv[1], "send-cost"))
    {
      write_config(valid);
      mock.send_delay_us = 12000;
      assert(command("start") == 0); wait_active();
      uint64_t until = now_ms() + 2500;
      while (now_ms() < until)
        {
          nxmutex_lock(&g.lock);
          assert(g.running && !g.error && !g.buffer_error && !g.tx_overflows);
          nxmutex_unlock(&g.lock);
          usleep(20000);
        }
      assert(command("stop") == 0); wait_idle(); assert_clean();
      assert(!g.error && !g.tx_overflows && g.send_calls >= 100);
      assert(atomic_load(&tx_sched) == 1 && atomic_load(&audio_sched) == 1);
      assert(g.send_max_ms >= 10);
      assert(unlink(AGORA_CONFIG) == 0);
      puts("Agora send-cost pacing PASS:12ms SDK cost inside20ms period, bounded queue and clean stop; SDK/audio mocked");
      return 0;
    }
  if (argc > 1 && !strcmp(argv[1], "slow-send"))
    {
      char text[256];
      write_config(valid);
      mock.send_delay_us = 60000;
      assert(command("start") == 0); wait_active(); wait_idle(); assert_clean();
      assert(g.error == -ENOBUFS && g.buffer_error == -ENOBUFS);
      assert(g.tx_overflows == 1 && g.rx_overflows == 0);
      assert(g.captured > g.up && g.send_calls > 0 && g.tx_high <= TX_SAMPLES);
      assert(g.capture_max == PCM_SAMPLES / 2 && g.tx_elapsed > 0);
      assert(g.send_max_ms >= 50 && g.send_gap_max_ms >= 50);
      assert(agora_service_command("stats", text, sizeof(text)) == 0);
      assert(strstr(text, "overflow_tx/rx=1/0") && strlen(text) < sizeof(text) - 1);
      assert(unlink(AGORA_CONFIG) == 0);
      puts("Agora slow-send diagnostics PASS: real coordinator/capture queue overflow, timing retained after clean shutdown; SDK/audio mocked");
      return 0;
    }
  if (argc > 1 && !strcmp(argv[1], "init-failure"))
    {
      write_config(valid);
      mock.init_error = -1;
      assert(command("init-test") == 0); wait_idle();
      assert(g.blocked && g.sdk_init_failed && !g.initialized &&
             g.sdk_error == -1 && g.error == -EIO);
      assert(g.local && g.config.app_id[0] && !g.tx.data && !g.rx.data);
      assert(atomic_load(&mock.fini) == 0 && atomic_load(&mock.join) == 0);
      int calls = atomic_load(&mock.init);
      mock.init_error = 0; /* A stale SDK flag could now return fake success. */
      assert(command("start") == -EIO && command("probe") == -EIO &&
             command("init-test") == -EIO && command("config {}") == -EIO);
      assert(atomic_load(&mock.init) == calls);
      assert(command("stop") == 0 && command("status") == 0 && g.blocked);
      assert(unlink(AGORA_CONFIG) == 0);
      puts("Agora partial SDK init failure PASS: retained inputs, no fini/re-entry, reset required");
      return 0;
    }
  if (argc > 1 && (!strcmp(argv[1], "join-failure") || !strcmp(argv[1], "lease-join-failure")))
    {
      write_config("{\"server_host\":\"127.0.0.1\",\"volume\":30}");
      bool lease_failure = !strcmp(argv[1], "lease-join-failure");
      if (lease_failure) mock.lease_join_error = true;
      else mock.join_error = EINVAL;
      assert(command("start") == 0); wait_active();
      wait_count(&mock.capture, 1);
      assert(command("stop") == 0); wait_idle();
      assert(g.blocked && (lease_failure ? g.lease_started : g.audio_started) &&
             g.initialized && g.cleanup_error == -EINVAL);
      assert(g.camera_paused && atomic_load(&mock.camera_pause) == 1 &&
             atomic_load(&mock.camera_resume) == 0);
      if (lease_failure) assert(!g.audio_started);
      assert(g.local && g.session && g.tx.data && g.rx.data && g.config.token[0]);
      assert(atomic_load(&mock.fini) == 0 && atomic_load(&mock.cloud_stop) == 1);
      assert(command("start") == -EIO);
      uint64_t deadline = now_ms() + 1000;
      while (atomic_load(&mock.owners) && now_ms() < deadline) usleep(1000);
      assert(atomic_load(&mock.owners) == 0);
      assert(unlink(AGORA_CONFIG) == 0);
      puts("Agora session mock worker join failure PASS: no SDK teardown/free; cloud stop attempted; restart blocked");
      return 0;
    }
  write_config(valid);
  unsigned int late = 0;
  assert(send_ready_at(120, 100) == 120);
  assert(send_ready_at(105, 100) == 110);
  assert(send_ready_at(0, 0) == 0);
  /* A12ms send starting at100 finishes112; next nominal start120, not122. */
  assert(send_ready_at(120, 100) < send_ready_at(120, 112));
  assert(wait_send_deadline(now_ms()) == 0);
  assert(advance_send(100, 103, &late) == 120);
  assert(advance_send(120, 126, &late) == 140); /* No per-send drift. */
  assert(advance_send(140, 221, &late) == 160 && late == 1);
  assert(advance_send(160, 231, &late) == 251 && late == 0); /* Bounded catch-up. */
  assert(command("version") == 0 && command("status") == 0);
  assert(command("stats") == 0);
  assert(command("bad-action") == -EINVAL);

  assert(command("probe") == 0); wait_idle(); assert_clean();
  assert(!g.error && atomic_load(&mock.join) == 1 && atomic_load(&mock.fini) == 1);
  assert(atomic_load(&mock.capture) == 0 && atomic_load(&mock.play) == 0);
  assert(command("init-test") == 0); wait_idle(); assert_clean();
  assert(atomic_load(&mock.join) == 1 && atomic_load(&mock.capture) == 0);
  assert(atomic_load(&mock.camera_pause) == 0);

  /* A camera close failure must prevent SDK/cloud/audio startup entirely. */
  mock.camera_pause_error = -EIO;
  int before_init = atomic_load(&mock.init);
  int before_cloud = atomic_load(&mock.cloud_start);
  assert(command("start") == 0); wait_idle();
  assert(g.error == -EIO && !g.camera_paused && !g.blocked);
  assert(atomic_load(&mock.init) == before_init &&
         atomic_load(&mock.cloud_start) == before_cloud &&
         atomic_load(&mock.camera_resume) == 0);
  mock.camera_pause_error = 0;
  /* The failed acquisition has no matching resume. Exclude it from counts. */
  atomic_fetch_sub(&mock.camera_pause, 1);
  assert_clean();

  mock.camera_resume_error = -EIO;
  assert(command("start") == 0); wait_active();
  assert(command("stop") == 0); wait_idle(); assert_clean();
  assert(g.cleanup_error == -EIO && !g.blocked);
  mock.camera_resume_error = 0;

  assert(command("start") == 0); wait_active();
  assert(command("start") == -EBUSY);
  assert(command("config {}") == -EBUSY);
  wait_count(&mock.send, 2);
  int captures = atomic_load(&mock.capture);
  emit_pcm(PCM_SAMPLES, AUDIO_DATA_TYPE_PCM);
  wait_count(&mock.play, 1); wait_count(&mock.complete, 1);
  wait_count(&mock.capture, captures + 1);
  assert(g.down == PCM_SAMPLES);
  assert(command("stop") == 0); wait_idle(); assert_clean(); assert(!g.error);
  assert(command("toggle") == 0); wait_active();
  assert(command("toggle") == 0); wait_idle(); assert_clean();

  mock.withhold_join = true;
  assert(command("probe") == 0); wait_idle(); assert_clean();
  assert(g.error == -ETIMEDOUT);
  int joins = atomic_load(&mock.join);
  assert(command("start") == 0); wait_count(&mock.join, joins + 1);
  mock.events->on_join_channel_success(43, 123, 0); /* Other connection ignored. */
  usleep(20000);
  nxmutex_lock(&g.lock); assert(!g.joined && g.phase == JOINING); nxmutex_unlock(&g.lock);
  mock.events->on_join_channel_success(42, 123, 0);
  wait_active(); assert(command("stop") == 0); wait_idle(); assert_clean();
  mock.withhold_join = false;
  write_config("{\"app_id\":\"not-an-app-id\"}");
  int initializations = atomic_load(&mock.init);
  assert(command("start") == 0); wait_idle(); assert_clean();
  assert(g.error == -EINVAL && atomic_load(&mock.init) == initializations);
  invalid_field("uid", "0"); invalid_field("uid", "4294967296");
  invalid_field("uid", "1.5"); invalid_field("remote_uid", "0");
  invalid_field("remote_uid", "123"); invalid_field("volume", "101");
  invalid_field("volume", "-1"); invalid_field("volume", "0.5");
  invalid_field("token", "\"too-short\"");
  invalid_field("app_id", "\"gggggggggggggggggggggggggggggggg\"");
  invalid_field("channel", "\"0123456789012345678901234567890123456789012345678901234567890123\"");
  write_config(valid);
  char provision[512];
  snprintf(provision, sizeof(provision), "config %s", valid);
  assert(command(provision) == 0);
  assert(command("start") == 0); wait_active();
  emit_pcm(RX_SAMPLES + 1, AUDIO_DATA_TYPE_PCM); wait_idle(); assert_clean();
  assert(g.buffer_error == -ENOBUFS);
  assert(command("start") == 0); wait_active();
  emit_pcm(PCM_SAMPLES, AUDIO_DATA_TYPE_G722); wait_idle(); assert_clean();
  assert(g.buffer_error == -EPROTO);
  mock.tx_overflow = true;
  assert(command("start") == 0); wait_idle(); assert_clean();
  assert(g.buffer_error == -ENOBUFS); mock.tx_overflow = false;

  assert(command("start") == 0); wait_active();
  mock.events->on_connection_lost(42); wait_idle(); assert_clean();
  assert(g.error == -ENETDOWN);

  write_config("{\"server_host\":\"127.0.0.1\",\"server_port\":8766,\"device_key\":\"mock-key\",\"volume\":30}");
  assert(command("start") == 0); wait_active();
  wait_count(&mock.keepalive, 1);
  assert(command("stop") == 0); wait_idle(); assert_clean();
  assert(atomic_load(&mock.cloud_stop) == 1);
  mock.keepalive_error = -ENETDOWN;
  assert(command("start") == 0); wait_idle(); assert_clean();
  assert(g.control_error == -ENETDOWN && atomic_load(&mock.cloud_stop) == 2);
  mock.keepalive_error = 0;

  int cloud_starts = atomic_load(&mock.cloud_start);
  write_config("{\"server_host\":\"127.0.0.1\",\"volume\":30.5}");
  assert(command("start") == 0); wait_idle(); assert_clean();
  assert(g.error == -EINVAL && atomic_load(&mock.cloud_start) == cloud_starts);
  write_config("{\"server_host\":\"127.0.0.1\",\"volume\":17}");
  mock.expected_volume = 17;
  assert(command("start") == 0); wait_active();
  emit_pcm(PCM_SAMPLES, AUDIO_DATA_TYPE_PCM);
  wait_count(&mock.complete, 2);
  assert(atomic_load(&mock.playback_volume) == 17);
  assert(command("stop") == 0); wait_idle(); assert_clean();
  mock.expected_volume = 0;

  /* HTTP takes longer than the 320ms TX queue, but RTC keeps sending. Stop
   * remains asynchronous and cleanup waits for the in-flight lease request.
   */
  write_config("{\"server_host\":\"127.0.0.1\",\"volume\":30}");
  mock.keepalive_delay_us = 600000;
  mock.send_delay_us = 3000;
  int renewals = atomic_load(&mock.keepalive);
  assert(command("start") == 0); wait_active();
  wait_count(&mock.keepalive, renewals + 1);
  assert(atomic_load(&mock.keepalive_busy));
  int sends = atomic_load(&mock.send), finishes = atomic_load(&mock.fini);
  wait_count(&mock.send, sends + 16); /* >320ms of ongoing TX during HTTP. */
  assert(atomic_load(&mock.keepalive_busy));
  nxmutex_lock(&g.lock); assert(!g.buffer_error && !g.error); nxmutex_unlock(&g.lock);
  uint64_t stop_at = now_ms();
  assert(command("stop") == 0 && now_ms() - stop_at < 50);
  usleep(15000);
  nxmutex_lock(&g.lock);
  assert(g.running && g.phase == STOPPING && g.local && g.tx.data);
  nxmutex_unlock(&g.lock);
  assert(atomic_load(&mock.fini) == finishes);
  wait_idle(); assert_clean(); assert(!g.buffer_error && !g.error);
  assert(!atomic_load(&mock.keepalive_busy));
  mock.keepalive_delay_us = mock.send_delay_us = 0;

  int stops = atomic_load(&mock.cloud_stop);
  mock.fini_error = -12;
  assert(command("start") == 0); wait_active();
  assert(command("stop") == 0); wait_idle();
  assert(g.blocked && g.initialized && g.cleanup_error == -12);
  assert(g.camera_paused && atomic_load(&mock.camera_pause) ==
         atomic_load(&mock.camera_resume) + 1);
  assert(g.local && g.session && g.tx.data && g.rx.data && g.config.token[0]);
  assert(strlen(mock.held_app) == 32 && strlen(mock.held_token) == 32);
  assert(atomic_load(&mock.allocations) == 2 && atomic_load(&mock.cloud_stop) == stops + 1);
  initializations = atomic_load(&mock.init);
  assert(command("start") == -EIO && command("probe") == -EIO && command("init-test") == -EIO);
  assert(command("config {}") == -EIO);
  assert(atomic_load(&mock.init) == initializations);
  emit_pcm(PCM_SAMPLES, AUDIO_DATA_TYPE_PCM); /* Late callback safely ignored. */
  assert(command("status") == 0);
  assert(unlink(AGORA_CONFIG) == 0);
  puts("Agora session mock tests PASS: probe/join, half duplex/COMPLETE, absolute pacing, delayed lease/TX, LAN volume, restart/errors, retained cleanup failure");
  return 0;
}
