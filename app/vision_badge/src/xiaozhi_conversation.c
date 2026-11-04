/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <netutils/cJSON.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vision_badge/services.h>
#include <vision_badge/mimo_http.h>
#include <vision_badge/xiaozhi_audio.h>
#include <vision_badge/xiaozhi_protocol.h>
#include <vision_badge/xiaozhi_transport.h>
#include <vision_badge/xiaozhi_conversation.h>

#define VOICE_CONFIG "/mnt/sdnand/prov/xiaozhi.cfg"
#define PCM_FRAMES 16
#define RX_FRAMES 32
#define RESPONSE_TIMEOUT 45000u

struct rx_frame_s
{
  enum xiaozhi_event_e event;
  size_t bytes;
  uint8_t packet[XIAOZHI_OPUS_MAX];
};

struct conversation_s
{
  mutex_t lock;
  struct xiaozhi_connection_s config;
  struct xiaozhi_session_s session; /* network owner only */
  struct xiaozhi_transport_s *wire;
  struct xiaozhi_playout_s *playout;
  pthread_t audio;
  pthread_t encoder;
  pthread_t decoder;
  bool stop;
  bool listening;
  bool speaking;
  bool restart;
  bool probe;
  bool hello;
  unsigned int volume;
  uint32_t epoch;
  uint32_t input_epoch;
  int error;
  uint64_t response_deadline;
  unsigned int pcm_head;
  unsigned int pcm_count;
  size_t partial_count;
  int16_t partial[XIAOZHI_FRAME_SAMPLES];
  int16_t pcm[PCM_FRAMES][XIAOZHI_FRAME_SAMPLES];
  unsigned int rx_head;
  unsigned int rx_count;
  struct rx_frame_s rx[RX_FRAMES];
  uint32_t encoded;
  uint32_t decoded;
  uint32_t turns;
};

static mutex_t g_lock = NXMUTEX_INITIALIZER;
static struct conversation_s *g_conversation;
static bool g_running;
static int g_last_error;

static uint64_t voice_now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void fail(struct conversation_s *ctx, int error)
{
  nxmutex_lock(&ctx->lock);
  if (ctx->error == 0)
    {
      ctx->error = error;
    }
  ctx->stop = true;
  ctx->listening = false;
  nxmutex_unlock(&ctx->lock);
}

static bool stopping(void *arg)
{
  struct conversation_s *ctx = arg;
  bool result;
  nxmutex_lock(&ctx->lock);
  result = ctx->stop;
  nxmutex_unlock(&ctx->lock);
  return result;
}

static bool capture_cancel(void *arg)
{
  struct conversation_s *ctx = arg;
  bool result;
  nxmutex_lock(&ctx->lock);
  result = ctx->stop || !ctx->listening;
  nxmutex_unlock(&ctx->lock);
  return result;
}

static int capture_sink(void *arg, const int16_t *pcm, size_t samples)
{
  struct conversation_s *ctx = arg;
  size_t count;
  unsigned int tail;
  int ret = 0;
  nxmutex_lock(&ctx->lock);
  while (samples != 0 && ctx->listening && !ctx->stop)
    {
      count = XIAOZHI_FRAME_SAMPLES - ctx->partial_count;
      if (count > samples)
        {
          count = samples;
        }
      memcpy(ctx->partial + ctx->partial_count, pcm, count * sizeof(*pcm));
      ctx->partial_count += count;
      pcm += count;
      samples -= count;
      if (ctx->partial_count == XIAOZHI_FRAME_SAMPLES)
        {
          if (ctx->pcm_count == PCM_FRAMES)
            {
              ret = -EOVERFLOW;
              break;
            }
          tail = (ctx->pcm_head + ctx->pcm_count) % PCM_FRAMES;
          memcpy(ctx->pcm[tail], ctx->partial, sizeof(ctx->partial));
          ctx->pcm_count++;
          ctx->partial_count = 0;
        }
    }
  nxmutex_unlock(&ctx->lock);
  return ret;
}

struct playback_s
{
  struct conversation_s *ctx;
  uint32_t epoch;
  size_t offset;
  size_t samples;
  int16_t pcm[XIAOZHI_FRAME_SAMPLES];
};

static int playback_source(void *arg, int16_t *pcm, size_t capacity)
{
  struct playback_s *play = arg;
  size_t count;
  int ret;
  if (play->offset == play->samples)
    {
      ret = xiaozhi_playout_pop(play->ctx->playout, play->epoch,
                               play->pcm, XIAOZHI_FRAME_SAMPLES);
      if (ret <= 0)
        {
          return ret;
        }
      play->samples = (size_t)ret;
      play->offset = 0;
    }
  count = play->samples - play->offset;
  if (count > capacity)
    {
      count = capacity;
    }
  memcpy(pcm, play->pcm + play->offset, count * sizeof(*pcm));
  play->offset += count;
  return (int)count;
}

static bool playback_cancel(void *arg)
{
  struct playback_s *play = arg;
  return stopping(play->ctx);
}

static void *audio_worker(void *arg)
{
  struct conversation_s *ctx = arg;
  struct playback_s play;
  bool listen;
  bool speak;
  uint32_t epoch;
  int ret;
  while (!stopping(ctx))
    {
      nxmutex_lock(&ctx->lock);
      listen = ctx->listening;
      speak = ctx->speaking;
      epoch = ctx->epoch;
      nxmutex_unlock(&ctx->lock);
      if (listen)
        {
          ret = audio_service_capture_stream(capture_sink, capture_cancel, ctx);
          if (ret < 0 && ret != -ECANCELED)
            {
              fail(ctx, ret);
            }
        }
      else if (speak)
        {
          memset(&play, 0, sizeof(play));
          play.ctx = ctx;
          play.epoch = epoch;
          ret = audio_service_play_stream(playback_source, playback_cancel,
                                          &play, ctx->volume);
          if (ret < 0)
            {
              if (ret != -ECANCELED)
                {
                  fail(ctx, ret);
                }
            }
          else
            {
              nxmutex_lock(&ctx->lock);
              ctx->speaking = false;
              ctx->restart = true; /* device COMPLETE, not TTS stop */
              ctx->turns++;
              nxmutex_unlock(&ctx->lock);
            }
        }
      else
        {
          usleep(5000);
        }
    }
  return NULL;
}

static void *encoder_worker(void *arg)
{
  struct conversation_s *ctx = arg;
  struct xiaozhi_codec_s *codec = NULL;
  uint32_t epoch = 0;
  uint32_t current;
  int16_t pcm[XIAOZHI_FRAME_SAMPLES];
  uint8_t packet[XIAOZHI_OPUS_MAX];
  size_t bytes;
  int ret = xiaozhi_codec_open(&codec);
  if (ret < 0)
    {
      fail(ctx, ret);
      return NULL;
    }
  while (!stopping(ctx))
    {
      nxmutex_lock(&ctx->lock);
      current = ctx->input_epoch;
      if (!ctx->listening || ctx->pcm_count == 0)
        {
          nxmutex_unlock(&ctx->lock);
          usleep(5000);
          continue;
        }
      memcpy(pcm, ctx->pcm[ctx->pcm_head], sizeof(pcm));
      ctx->pcm_head = (ctx->pcm_head + 1) % PCM_FRAMES;
      ctx->pcm_count--;
      nxmutex_unlock(&ctx->lock);
      if (epoch != current)
        {
          ret = xiaozhi_codec_reset(codec);
          epoch = current;
          if (ret < 0)
            {
              fail(ctx, ret);
              break;
            }
        }
      ret = xiaozhi_codec_encode(codec, pcm, XIAOZHI_FRAME_SAMPLES,
                                 packet, sizeof(packet));
      if (ret < 0)
        {
          fail(ctx, ret);
          break;
        }
      bytes = (size_t)ret;
      nxmutex_lock(&ctx->lock);
      /* Discard work from a capture generation stopped by STT/TTS.
       * Capture never waits on this socket or performs encoding itself.
       */
      if (ctx->listening && !ctx->stop && current == ctx->input_epoch)
        {
          ret = xiaozhi_transport_send(ctx->wire, true, packet, bytes);
          if (ret == 0)
            {
              ctx->encoded++;
            }
        }
      else
        {
          ret = 0;
        }
      nxmutex_unlock(&ctx->lock);
      if (ret < 0)
        {
          fail(ctx, ret); /* bounded TX full is a recoverable session failure */
        }
    }
  xiaozhi_codec_close(codec);
  return NULL;
}

static void *decoder_worker(void *arg)
{
  struct conversation_s *ctx = arg;
  struct xiaozhi_codec_s *codec = NULL;
  struct rx_frame_s frame;
  int16_t pcm[XIAOZHI_FRAME_SAMPLES];
  uint32_t epoch = 0;
  uint64_t deadline;
  int ret = xiaozhi_codec_open(&codec);
  if (ret < 0)
    {
      fail(ctx, ret);
      return NULL;
    }
  while (!stopping(ctx))
    {
      nxmutex_lock(&ctx->lock);
      if (ctx->rx_count == 0)
        {
          nxmutex_unlock(&ctx->lock);
          usleep(5000);
          continue;
        }
      frame = ctx->rx[ctx->rx_head];
      ctx->rx_head = (ctx->rx_head + 1) % RX_FRAMES;
      ctx->rx_count--;
      nxmutex_unlock(&ctx->lock);
      if (frame.event == XIAOZHI_EVENT_TTS_START)
        {
          ret = xiaozhi_codec_reset(codec);
          if (ret < 0)
            {
              fail(ctx, ret);
              break;
            }
          epoch = xiaozhi_playout_begin(ctx->playout);
          nxmutex_lock(&ctx->lock);
          ctx->epoch = epoch;
          ctx->speaking = true;
          nxmutex_unlock(&ctx->lock);
        }
      else if (frame.event == XIAOZHI_EVENT_TTS_STOP)
        {
          ret = xiaozhi_playout_finish(ctx->playout, epoch);
          if (ret < 0)
            {
              fail(ctx, ret);
            }
        }
      else
        {
          ret = xiaozhi_codec_decode(codec, frame.packet, frame.bytes,
                                     pcm, XIAOZHI_FRAME_SAMPLES);
          if (ret < 0 || epoch == 0)
            {
              fail(ctx, ret < 0 ? ret : -EPROTO);
              break;
            }
          deadline = voice_now() + 2000;
          do
            {
              ret = xiaozhi_playout_push(ctx->playout, epoch, pcm,
                                         XIAOZHI_FRAME_SAMPLES);
              if (ret == -EAGAIN)
                {
                  usleep(1000);
                }
            }
          while (ret == -EAGAIN && !stopping(ctx) && voice_now() < deadline);
          if (ret < 0 && !stopping(ctx))
            {
              fail(ctx, ret);
            }
          nxmutex_lock(&ctx->lock);
          ctx->decoded++;
          nxmutex_unlock(&ctx->lock);
        }
    }
  xiaozhi_codec_close(codec);
  return NULL;
}

static int start_listen(struct conversation_s *ctx)
{
  char text[512];
  int ret = xiaozhi_build_listen(&ctx->session, true, text, sizeof(text));
  if (ret > 0)
    {
      ret = xiaozhi_transport_send(ctx->wire, false, text, (size_t)ret);
    }
  if (ret == 0)
    {
      nxmutex_lock(&ctx->lock);
      ctx->listening = true;
      ctx->restart = false;
      ctx->response_deadline = 0;
      ctx->pcm_count = 0;
      ctx->partial_count = 0;
      ctx->input_epoch++;
      nxmutex_unlock(&ctx->lock);
    }
  return ret;
}

static int wire_receive(void *arg, enum xiaozhi_wire_event_e event,
                         const void *data, size_t bytes)
{
  struct conversation_s *ctx = arg;
  struct rx_frame_s *frame;
  enum xiaozhi_event_e control = XIAOZHI_EVENT_IGNORE;
  char hello[512];
  int ret;
  if (event == XIAOZHI_WIRE_OPEN)
    {
      ret = xiaozhi_build_hello(hello, sizeof(hello));
      return ret > 0 ? xiaozhi_transport_send(ctx->wire, false, hello, ret) : ret;
    }
  if (event == XIAOZHI_WIRE_CLOSED)
    {
      if (!stopping(ctx))
        {
          fail(ctx, -ECONNRESET);
        }
      return 0;
    }
  if (event == XIAOZHI_WIRE_TEXT)
    {
      ret = xiaozhi_parse_control(&ctx->session, data, bytes, &control);
      if (ret < 0)
        {
          return ret;
        }
      if (control == XIAOZHI_EVENT_HELLO)
        {
          ctx->hello = true;
          return ctx->probe ? 0 : start_listen(ctx);
        }
      if (control == XIAOZHI_EVENT_STT || control == XIAOZHI_EVENT_TTS_START)
        {
          nxmutex_lock(&ctx->lock);
          ctx->listening = false;
          ctx->pcm_count = 0;
          ctx->partial_count = 0;
          ctx->response_deadline = voice_now() + RESPONSE_TIMEOUT;
          nxmutex_unlock(&ctx->lock);
        }
      if (control != XIAOZHI_EVENT_TTS_START && control != XIAOZHI_EVENT_TTS_STOP)
        {
          return 0;
        }
    }
  nxmutex_lock(&ctx->lock);
  if (ctx->rx_count == RX_FRAMES)
    {
      nxmutex_unlock(&ctx->lock);
      return -EAGAIN;
    }
  frame = &ctx->rx[(ctx->rx_head + ctx->rx_count) % RX_FRAMES];
  frame->event = control;
  frame->bytes = bytes;
  if (event == XIAOZHI_WIRE_BINARY)
    {
      if (bytes == 0 || bytes > sizeof(frame->packet) || !ctx->hello)
        {
          nxmutex_unlock(&ctx->lock);
          return -EPROTO;
        }
      memcpy(frame->packet, data, bytes);
    }
  ctx->rx_count++;
  nxmutex_unlock(&ctx->lock);
  return 0;
}

static int spawn(pthread_t *thread, void *(*worker)(void *),
                  struct conversation_s *ctx, size_t stack)
{
  pthread_attr_t attr;
  int ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      ret = pthread_attr_setstacksize(&attr, stack);
      if (ret == 0)
        {
          ret = pthread_create(thread, &attr, worker, ctx);
        }
      pthread_attr_destroy(&attr);
    }
  return -ret;
}

static void *connection_worker(void *arg)
{
  struct conversation_s *ctx = arg;
  bool audio = false;
  bool encoder = false;
  bool decoder = false;
  bool restart;
  bool speaking;
  uint64_t deadline;
  uint64_t hello_deadline = 0;
  int ret = mimo_http_prepare_network();
  if (ret == 0 && ctx->config.tls)
    {
      ret = mimo_http_sync_clock();
    }
  if (ret == 0 && !stopping(ctx))
    {
      ret = xiaozhi_playout_open(&ctx->playout);
    }
  if (ret == 0 && !stopping(ctx))
    {
      ret = xiaozhi_transport_open(&ctx->wire, &ctx->config, wire_receive, ctx);
      hello_deadline = voice_now() + 12000;
    }
  if (ret == 0 && !ctx->probe && !stopping(ctx))
    {
      ret = spawn(&ctx->encoder, encoder_worker, ctx, 102400);
      encoder = ret == 0;
      if (ret == 0)
        {
          ret = spawn(&ctx->decoder, decoder_worker, ctx, 102400);
          decoder = ret == 0;
        }
      if (ret == 0)
        {
          ret = spawn(&ctx->audio, audio_worker, ctx, 8192);
          audio = ret == 0;
        }
    }
  while (ret == 0 && !stopping(ctx))
    {
      ret = xiaozhi_transport_service(ctx->wire);
      if (ctx->probe && ctx->hello)
        {
          break;
        }
      nxmutex_lock(&ctx->lock);
      restart = ctx->restart;
      speaking = ctx->speaking;
      deadline = ctx->response_deadline;
      nxmutex_unlock(&ctx->lock);
      if (ret == 0 && restart)
        {
          ret = start_listen(ctx);
        }
      if (ret == 0 && ((!ctx->hello && voice_now() > hello_deadline) ||
          (deadline != 0 && !speaking && !restart && voice_now() > deadline)))
        {
          ret = -ETIMEDOUT;
        }
    }
  nxmutex_lock(&ctx->lock);
  if (ret < 0 && ret != -ECANCELED && ctx->error == 0)
    {
      ctx->error = ret;
    }
  ctx->stop = true;
  ctx->listening = false;
  nxmutex_unlock(&ctx->lock);
  if (audio)
    {
      pthread_join(ctx->audio, NULL);
    }
  if (encoder)
    {
      pthread_join(ctx->encoder, NULL);
    }
  if (decoder)
    {
      pthread_join(ctx->decoder, NULL);
    }
  /* CLOSED during explicit teardown must not turn user stop into failure. */
  ret = ctx->error;
  xiaozhi_transport_close(ctx->wire);
  xiaozhi_playout_close(ctx->playout);
  printf("R1 voice: stopped error=%d encoded=%lu decoded=%lu turns=%lu\n",
         ret, (unsigned long)ctx->encoded, (unsigned long)ctx->decoded,
         (unsigned long)ctx->turns);
  nxmutex_lock(&g_lock);
  g_last_error = ret;
  g_running = false;
  g_conversation = NULL;
  nxmutex_unlock(&g_lock);
  nxmutex_destroy(&ctx->lock);
  memset(&ctx->config, 0, sizeof(ctx->config));
  free(ctx);
  return NULL;
}

static bool json_string(cJSON *root, const char *name, char *dest, size_t size,
                         bool required)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  if (item == NULL && !required)
    {
      return true;
    }
  if (!cJSON_IsString(item) || strlen(item->valuestring) >= size ||
      strpbrk(item->valuestring, "\r\n") != NULL)
    {
      return false;
    }
  strcpy(dest, item->valuestring);
  return true;
}

static int read_config(struct conversation_s *ctx)
{
  char input[2049];
  cJSON *root;
  cJSON *port;
  cJSON *tls;
  cJSON *volume;
  FILE *file = fopen(VOICE_CONFIG, "rb");
  size_t bytes;
  int ret = -EINVAL;
  if (file == NULL)
    {
      return -errno;
    }
  bytes = fread(input, 1, sizeof(input), file);
  if (ferror(file) || bytes == 0 || bytes >= sizeof(input) ||
      memchr(input, '\0', bytes) != NULL)
    {
      fclose(file);
      return -EBADMSG;
    }
  fclose(file);
  input[bytes] = '\0';
  root = cJSON_ParseWithLengthOpts(input, bytes + 1, NULL, true);
  memset(input, 0, sizeof(input));
  if (!cJSON_IsObject(root))
    {
      cJSON_Delete(root);
      return -EBADMSG;
    }
  port = cJSON_GetObjectItemCaseSensitive(root, "port");
  tls = cJSON_GetObjectItemCaseSensitive(root, "tls");
  volume = cJSON_GetObjectItemCaseSensitive(root, "volume");
  ctx->volume = 30;
  strcpy(ctx->config.path, "/xiaozhi/v1/");
  strcpy(ctx->config.device, "r1-441");
  if (cJSON_IsNumber(port) && port->valuedouble == port->valueint &&
      port->valueint > 0 && port->valueint <= 65535 &&
      (tls == NULL || cJSON_IsBool(tls)) &&
      (volume == NULL || (cJSON_IsNumber(volume) && volume->valueint >= 0 &&
       volume->valueint <= 100 && volume->valuedouble == volume->valueint)) &&
      json_string(root, "host", ctx->config.host, sizeof(ctx->config.host), true) &&
      json_string(root, "path", ctx->config.path, sizeof(ctx->config.path), false) &&
      json_string(root, "device", ctx->config.device, sizeof(ctx->config.device), false) &&
      json_string(root, "token", ctx->config.token, sizeof(ctx->config.token), false) &&
      json_string(root, "ca_file", ctx->config.ca_file, sizeof(ctx->config.ca_file), false))
    {
      ctx->config.port = port->valueint;
      ctx->config.tls = cJSON_IsTrue(tls);
      if (volume != NULL)
        {
          ctx->volume = volume->valueint;
        }
      ret = 0;
    }
  cJSON_Delete(root);
  return ret;
}

static int save_config(const char *action)
{
  char host[65];
  char token[65] = {0};
  unsigned int port;
  char extra;
  cJSON *root;
  char *json;
  FILE *file;
  int count;
  int ret;
  count = sscanf(action, "config %64s %u %64s %c", host, &port, token, &extra);
  if ((count != 2 && count != 3) || port == 0 || port > 65535 ||
      strpbrk(host, "\"\\\r\n") != NULL)
    {
      return -EINVAL;
    }
  root = cJSON_CreateObject();
  if (root == NULL)
    {
      return -ENOMEM;
    }
  if (!cJSON_AddStringToObject(root, "host", host) ||
      !cJSON_AddNumberToObject(root, "port", port) ||
      !cJSON_AddStringToObject(root, "token", token) ||
      !cJSON_AddBoolToObject(root, "tls", false) ||
      !cJSON_AddNumberToObject(root, "volume", 30))
    {
      cJSON_Delete(root);
      return -ENOMEM;
    }
  json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (json == NULL)
    {
      return -ENOMEM;
    }
  if (mkdir("/mnt/sdnand/prov", 0700) < 0 && errno != EEXIST)
    {
      memset(json, 0, strlen(json));
      free(json);
      memset(token, 0, sizeof(token));
      return -errno;
    }
  /* FAT 8.3-safe names, unlike xiaozhi.json.tmp. */
  file = fopen("/mnt/sdnand/prov/voice.tmp", "wb");
  if (file == NULL)
    {
      ret = -errno;
    }
  else
    {
      size_t bytes = strlen(json);
      ret = fwrite(json, 1, bytes, file) == bytes ? 0 : -EIO;
      if (fclose(file) != 0 && ret == 0)
        {
          ret = -errno;
        }
      if (ret == 0 && rename("/mnt/sdnand/prov/voice.tmp", VOICE_CONFIG) < 0)
        {
          ret = -errno;
        }
    }
  memset(json, 0, strlen(json));
  free(json);
  memset(token, 0, sizeof(token));
  return ret;
}

int xiaozhi_conversation_command(const char *action, char *summary, size_t size)
{
  struct conversation_s *ctx;
  pthread_t thread;
  bool probe;
  int ret = 0;
  if (action == NULL || summary == NULL || size == 0)
    {
      return -EINVAL;
    }
  nxmutex_lock(&g_lock);
  if (strcmp(action, "status") == 0)
    {
      if (g_conversation != NULL)
        {
          ctx = g_conversation;
          nxmutex_lock(&ctx->lock);
          snprintf(summary, size, "voice %s error=%d tx=%lu rx=%lu turns=%lu",
                   ctx->stop ? "stopping" : ctx->speaking ? "speaking" :
                   ctx->listening ? "listening" : "connecting/thinking",
                   ctx->error, (unsigned long)ctx->encoded,
                   (unsigned long)ctx->decoded, (unsigned long)ctx->turns);
          nxmutex_unlock(&ctx->lock);
        }
      else
        {
          snprintf(summary, size, "voice stopped last_error=%d", g_last_error);
        }
      goto out;
    }
  if (strncmp(action, "config ", 7) == 0)
    {
      ret = g_running ? -EBUSY : save_config(action);
      snprintf(summary, size, "voice configuration saved=%s error=%d",
               ret == 0 ? "yes" : "no", ret);
      goto out;
    }
  if (strcmp(action, "stop") == 0 ||
      (strcmp(action, "toggle") == 0 && g_running))
    {
      if (g_conversation != NULL)
        {
          nxmutex_lock(&g_conversation->lock);
          g_conversation->stop = true;
          g_conversation->listening = false;
          nxmutex_unlock(&g_conversation->lock);
        }
      snprintf(summary, size, "voice stop requested");
      goto out;
    }
  probe = strcmp(action, "probe") == 0;
  if (!probe && strcmp(action, "start") != 0 && strcmp(action, "toggle") != 0)
    {
      ret = -EINVAL;
      goto out;
    }
  if (g_running)
    {
      ret = -EALREADY;
      goto out;
    }
  ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }
  ret = read_config(ctx);
  if (ret == 0)
    {
      ret = nxmutex_init(&ctx->lock);
      if (ret == 0)
        {
          ctx->probe = probe;
          g_conversation = ctx;
          g_running = true;
          ret = spawn(&thread, connection_worker, ctx, 32768);
          if (ret == 0)
            {
              pthread_detach(thread);
            }
          else
            {
              g_conversation = NULL;
              g_running = false;
              nxmutex_destroy(&ctx->lock);
            }
        }
    }
  if (ret < 0)
    {
      memset(&ctx->config, 0, sizeof(ctx->config));
      free(ctx);
      snprintf(summary, size, "voice start failed=%d; configure server first", ret);
    }
  else
    {
      snprintf(summary, size, "%s started in background", probe ? "silent probe" : "conversation");
    }
out:
  nxmutex_unlock(&g_lock);
  return ret;
}
