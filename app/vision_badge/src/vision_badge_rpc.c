/* SPDX-License-Identifier: Apache-2.0 */

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/rpmsg/rpmsg.h>
#include <nuttx/semaphore.h>
#include <nuttx/signal.h>

#include <arch/chip/bk7258_rptun.h>

#include <vision_badge/rpc.h>

#ifdef CONFIG_BK7258_AP_CORE
#  ifdef CONFIG_BK7258_AIDK_BATTERY
#    include <fcntl.h>
#    include <nuttx/fs/fs.h>
#    include <nuttx/power/battery_ioctl.h>
#  endif
#  include <sched.h>
#  include <stdlib.h>
#  include <syslog.h>
#  include <nuttx/kthread.h>
#  include <vision_badge/config.h>
#  include <vision_badge/mimo_http.h>
#  include <vision_badge/services.h>
#  include <vision_badge/workflow.h>
#  ifdef CONFIG_CONTEST2026_441_POWER
#    include <vision_badge/power_service.h>
#  endif
#  ifdef CONFIG_CONTEST2026_441_WAKE
#    include <vision_badge/wake_service.h>
#  endif
#  ifdef CONFIG_CONTEST2026_441_XIAOZHI
#    include <vision_badge/xiaozhi_audio.h>
#    include <vision_badge/xiaozhi_conversation.h>
#  endif
#  ifdef CONFIG_CONTEST2026_441_AGORA
#    include <vision_badge/agora_service.h>
#    include <vision_badge/agora_control.h>
#  endif
#else
#  include <syslog.h>
#  include <nuttx/kthread.h>
#  include <nuttx/input/buttons.h>
#  include <arch/board/board.h>
#  include <unistd.h>

extern uint32_t board_buttons(void);
#endif

#define VISION_BADGE_RPC_ENDPOINT       "vision-badge"
#define VISION_BADGE_RPC_MAGIC          0x47444256u /* "VBDG" */
#define VISION_BADGE_RPC_VERSION        1u
#define VISION_BADGE_RPC_PROMPT_SIZE    160
#define VISION_BADGE_RPC_SEND_MS        200u

#ifdef CONFIG_BK7258_AP_CORE
#  define VISION_BADGE_RPC_REMOTE_NAME  "cp"
/* Fixed-point Opus uses variable-length stack scratch. Keep the workspace's
 * LIB_OPUS_STACKSIZE default budget for the diagnostic worker until measured
 * on R1; do not run it on the old 16 KiB camera-only stack.
 */
#  ifdef CONFIG_CONTEST2026_441_XIAOZHI
#    define VISION_BADGE_RPC_STACKSIZE  102400
#  else
/* Agora owns separate workers; this RPC task no longer runs Opus scratch. */
#    define VISION_BADGE_RPC_STACKSIZE  32768
#  endif
#  define VISION_BADGE_RPC_PRIORITY     99
#  define VISION_BADGE_NET_STACKSIZE    8192
#  define VISION_BADGE_NET_PRIORITY     110
#else
#  define VISION_BADGE_RPC_REMOTE_NAME  "ap"
#endif

enum vision_badge_rpc_command_e
{
  VISION_BADGE_RPC_QUERY = 1,
  VISION_BADGE_RPC_RESULT,
  VISION_BADGE_RPC_PROVISION,
  VISION_BADGE_RPC_AUDIO_LOOPBACK,
  VISION_BADGE_RPC_CODEC_SELFTEST,
  VISION_BADGE_RPC_CONVERSATION,
  VISION_BADGE_RPC_AGORA,
  VISION_BADGE_RPC_BATTERY,
  VISION_BADGE_RPC_AUDIO_DIAGNOSTIC,
  VISION_BADGE_RPC_CAMERA
#ifdef CONFIG_CONTEST2026_441_POWER
  , VISION_BADGE_RPC_POWER
#endif
};

struct vision_badge_rpc_wire_s
{
  uint32_t magic;
  uint16_t version;
  uint16_t command;
  uint32_t generation;
  uint32_t sequence;
  int32_t status;
  int32_t stage;
  char prompt[VISION_BADGE_RPC_PROMPT_SIZE];
  char text[VISION_BADGE_RPC_TEXT_SIZE];
};

struct vision_badge_rpc_dev_s
{
  struct rpmsg_endpoint endpoint;
  volatile bool initialized;
  volatile bool endpoint_created;
  volatile int connection_error;
  sem_t semaphore;
#ifdef CONFIG_BK7258_AP_CORE
  volatile bool pending;
  struct vision_badge_rpc_wire_s request;
#else
  mutex_t lock;
  uint32_t next_sequence;
  volatile uint32_t waiting_sequence;
  volatile bool reply_valid;
  struct vision_badge_rpc_result_s reply;
#endif
};

static struct vision_badge_rpc_dev_s g_vision_badge_rpc =
{
#ifndef CONFIG_BK7258_AP_CORE
  .lock = NXMUTEX_INITIALIZER,
#endif
};

static bool vision_badge_rpc_endpoint_ready(void)
{
  struct vision_badge_rpc_dev_s *priv = &g_vision_badge_rpc;
  volatile struct bk7258_rptun_control_s *control =
    bk7258_rptun_control();

  return __atomic_load_n(&priv->endpoint_created, __ATOMIC_ACQUIRE) &&
         control->state == BK7258_RPTUN_STATE_CONNECTED &&
         is_rpmsg_ept_ready(&priv->endpoint);
}

bool vision_badge_rpc_ready(void)
{
  return vision_badge_rpc_endpoint_ready();
}

static int vision_badge_rpc_send(const struct vision_badge_rpc_wire_s *msg,
                                 unsigned int timeout_ms)
{
  struct vision_badge_rpc_dev_s *priv = &g_vision_badge_rpc;
  clock_t started = clock_systime_ticks();
  clock_t limit = MSEC2TICK(timeout_ms);
  int ret = -ENOTCONN;

  do
    {
      if (!vision_badge_rpc_endpoint_ready())
        {
          return -ENOTCONN;
        }

      ret = rpmsg_trysend(&priv->endpoint, msg, sizeof(*msg));
      if (ret >= 0)
        {
          return 0;
        }

      if (ret != -ENOMEM && ret != -EAGAIN)
        {
          return ret;
        }

      nxsig_usleep(1000);
    }
  while ((clock_t)(clock_systime_ticks() - started) < limit);

  return -ETIMEDOUT;
}

static bool vision_badge_rpc_wire_valid(
  const struct vision_badge_rpc_wire_s *msg, size_t len)
{
  return msg != NULL && len == sizeof(*msg) &&
         msg->magic == VISION_BADGE_RPC_MAGIC &&
         msg->version == VISION_BADGE_RPC_VERSION &&
         (msg->command == VISION_BADGE_RPC_QUERY ||
          msg->command == VISION_BADGE_RPC_RESULT ||
          msg->command == VISION_BADGE_RPC_PROVISION ||
          msg->command == VISION_BADGE_RPC_AUDIO_LOOPBACK ||
          msg->command == VISION_BADGE_RPC_CODEC_SELFTEST ||
          msg->command == VISION_BADGE_RPC_CONVERSATION ||
          msg->command == VISION_BADGE_RPC_AGORA ||
          msg->command == VISION_BADGE_RPC_BATTERY ||
          msg->command == VISION_BADGE_RPC_AUDIO_DIAGNOSTIC ||
          msg->command == VISION_BADGE_RPC_CAMERA
#ifdef CONFIG_CONTEST2026_441_POWER
          || msg->command == VISION_BADGE_RPC_POWER
#endif
          );
}

#ifdef CONFIG_BK7258_AP_CORE
#ifdef CONFIG_CONTEST2026_441_POWER
static bool g_network_power_busy;
bool vision_badge_power_network_idle(void)
{
  return !__atomic_load_n(&g_network_power_busy, __ATOMIC_ACQUIRE);
}
#endif
static int vision_badge_network_worker(int argc, char *argv[])
{
  int ret;
  unsigned int attempt;

  (void)argc;
  (void)argv;

  /* Warm up the one-time Wi-Fi session and certificate clock after deferred
   * board bring-up, so the first user request does not pay that cost. */
  for (attempt = 0; attempt < 3; attempt++)
    {
#ifdef CONFIG_CONTEST2026_441_POWER
      if (!power_service_work_allowed()) break;
#endif
      ret = mimo_http_prepare_session();
      if (ret == 0)
        {
          syslog(LOG_INFO, "VISION BADGE network session ready\n");
#ifdef CONFIG_CONTEST2026_441_PRODUCT_STARTUP
          if (power_service_work_allowed())
            {
              char detail[192];
              ret = agora_service_command("camera-live-on", detail, sizeof(detail));
              if (!ret && power_service_work_allowed())
                ret = wake_service_command("start", detail, sizeof(detail));
              syslog(ret < 0 ? LOG_WARNING : LOG_INFO,
                     "VISION BADGE product startup camera/wake ret=%d; no RTC start requested\n", ret);
            }
#endif
#ifdef CONFIG_CONTEST2026_441_POWER
          __atomic_store_n(&g_network_power_busy, false, __ATOMIC_RELEASE);
#endif
          return 0;
        }

      syslog(LOG_WARNING,
             "VISION BADGE network warmup attempt=%u ret=%d\n",
             attempt + 1, ret);
      nxsig_usleep(2000000);
    }

  syslog(LOG_ERR, "VISION BADGE network warmup failed\n");
#ifdef CONFIG_CONTEST2026_441_POWER
  __atomic_store_n(&g_network_power_busy, false, __ATOMIC_RELEASE);
#endif
  return 0;
}

static bool vision_badge_rpc_copy_result(char *destination,
                                         size_t capacity,
                                         const char *source,
                                         size_t source_length)
{
  static const char ellipsis[] = "\xe2\x80\xa6";
  size_t keep;

  if (source_length < capacity)
    {
      memcpy(destination, source, source_length);
      destination[source_length] = '\0';
      return false;
    }

  keep = capacity - sizeof(ellipsis);
  while (keep > 0 &&
         ((unsigned char)source[keep] & 0xc0u) == 0x80u)
    {
      keep--;
    }

  memcpy(destination, source, keep);
  memcpy(destination + keep, ellipsis, sizeof(ellipsis));
  return true;
}
#endif

#ifdef CONFIG_BK7258_AP_CORE
static int vision_badge_battery_status(char *text, size_t capacity)
{
#ifdef CONFIG_BK7258_AIDK_BATTERY
  struct file battery;
  const char *step = "open";
  const char *state_name;
  int state = BATTERY_UNKNOWN;
  int millivolts = 0;
  int close_ret;
  int ret;

  ret = file_open(&battery, "/dev/bat0", O_RDONLY);
  if (ret >= 0)
    {
      step = "state";
      ret = file_ioctl(&battery, BATIOC_STATE, (unsigned long)&state);
      if (ret >= 0)
        {
          step = "voltage";
          ret = file_ioctl(&battery, BATIOC_GET_VOLTAGE,
                           (unsigned long)&millivolts);
        }

      close_ret = file_close(&battery);
      if (ret >= 0 && close_ret < 0)
        {
          step = "close";
          ret = close_ret;
        }
    }

  if (ret < 0)
    {
      snprintf(text, capacity, "battery: step=%s error=%d", step, ret);
      return ret;
    }

  switch (state)
    {
      case BATTERY_DISCHARGING: state_name = "discharging"; break;
      case BATTERY_CHARGING: state_name = "charging"; break;
      case BATTERY_FULL: state_name = "full"; break;
      default: state_name = "unknown"; break;
    }

  /* No fuel gauge or programmable charge controller is present.  Report
   * the measured voltage and pin-derived state, not invented percentages.
   */
  snprintf(text, capacity, "battery: voltage=%d mV state=%s",
           millivolts, state_name);
  return 0;
#else
  snprintf(text, capacity, "battery: not enabled on AP");
  return -ENOSYS;
#endif
}

static int vision_badge_rpc_worker(int argc, char *argv[])
{
  struct vision_badge_rpc_dev_s *priv = &g_vision_badge_rpc;
  struct vision_badge_rpc_wire_s request;
  struct vision_badge_rpc_wire_s response;
  struct vision_badge_context_s context;
  struct vision_badge_image_s image;
  struct vision_badge_result_s result;
  int ret;

  (void)argc;
  (void)argv;

#if defined(CONFIG_CONTEST2026_441_POWER) && defined(CONFIG_CONTEST2026_441_AGORA)
  unsigned int volume_level;
  ret = agora_control_volume_load(&volume_level);
  if (!ret)
    {
      char action[24], detail[128];
      if (volume_level) snprintf(action, sizeof(action), "volume %u", volume_level);
      else snprintf(action, sizeof(action), "volume reset");
      ret = vision_badge_audio_diagnostic(action, detail, sizeof(detail));
    }
  syslog(ret < 0 && ret != -ENOENT ? LOG_WARNING : LOG_INFO,
         "vision_badge: volume restore ret=%d; absent/invalid storage keeps configured default\n", ret);
#endif
  char camera_status[128];
  ret = camera_service_control("start", camera_status, sizeof(camera_status));
  syslog(ret < 0 ? LOG_WARNING : LOG_INFO,
         "vision_badge: camera startup ret=%d %s\n", ret, camera_status);

  for (;;)
    {
      ret = nxsem_wait_uninterruptible(&priv->semaphore);
      if (ret < 0)
        {
          continue;
        }

      if (!__atomic_load_n(&priv->pending, __ATOMIC_ACQUIRE))
        {
          continue;
        }

      __asm volatile ("dmb sy" ::: "memory");
      memcpy(&request, &priv->request, sizeof(request));
      memset(&priv->request, 0, sizeof(priv->request));
      __atomic_store_n(&priv->pending, false, __ATOMIC_RELEASE);
      memset(&response, 0, sizeof(response));
      response.magic = VISION_BADGE_RPC_MAGIC;
      response.version = VISION_BADGE_RPC_VERSION;
      response.command = VISION_BADGE_RPC_RESULT;
      response.generation = request.generation;
      response.sequence = request.sequence;

#ifdef CONFIG_CONTEST2026_441_WAKE
      /* Observation must not stop/restart MIC and camera owners. Mutating
       * camera/power commands still pass through the exclusive drain gate. */
      bool wake_exclusive=!(request.command==VISION_BADGE_RPC_BATTERY ||
        (request.command==VISION_BADGE_RPC_CAMERA && !strcmp(request.prompt,"status")) ||
#ifdef CONFIG_CONTEST2026_441_POWER
        (request.command==VISION_BADGE_RPC_POWER && !strcmp(request.prompt,"status")) ||
#endif
        (request.command==VISION_BADGE_RPC_AUDIO_DIAGNOSTIC &&
         (!strcmp(request.prompt,"volume") || !strncmp(request.prompt,"volume ",7))) ||
        (request.command==VISION_BADGE_RPC_AGORA &&
         (!strcmp(request.prompt,"status") || !strcmp(request.prompt,"stats") ||
          !strcmp(request.prompt,"visionstats") || !strncmp(request.prompt,"wake-",5))));
      int wake_admission=wake_service_rpc_begin(wake_exclusive);
      if (wake_admission<0)
        { ret=wake_admission; snprintf(response.text,sizeof(response.text),"wake: resource owner still draining or retained"); }
      else
#endif
#ifdef CONFIG_CONTEST2026_441_POWER
      if (request.command == VISION_BADGE_RPC_POWER)
        ret = power_service_command(request.prompt, response.text, sizeof(response.text));
      else if (!power_service_work_allowed() &&
               request.command != VISION_BADGE_RPC_BATTERY &&
               !(request.command == VISION_BADGE_RPC_AGORA &&
                 (!strcmp(request.prompt, "status") || !strcmp(request.prompt, "visionstats") ||
                  !strcmp(request.prompt, "stats") || !strcmp(request.prompt, "stop")
#ifdef CONFIG_CONTEST2026_441_WAKE
                  || !strcmp(request.prompt,"wake-status") || !strcmp(request.prompt,"wake-stop")
#endif
                  )))
        {
          ret = -ESHUTDOWN;
          snprintf(response.text, sizeof(response.text), "power: new work blocked; use power status/resume");
        }
      else
#endif
      if (request.command == VISION_BADGE_RPC_AGORA)
        {
#ifdef CONFIG_CONTEST2026_441_AGORA
          if (!strcmp(request.prompt, "config"))
            {
              /* Keep the existing wire size: JSON uses the 256-byte text
               * field rather than overflowing the 160-byte prompt field. */
              char action[VISION_BADGE_RPC_TEXT_SIZE + 7];
              snprintf(action, sizeof(action), "config %s", request.text);
              ret = agora_service_command(action, response.text,
                                          sizeof(response.text));
              explicit_bzero(action, sizeof(action));
            }
          else
            {
              ret = agora_service_command(request.prompt, response.text,
                                          sizeof(response.text));
            }
#else
          ret = -ENOSYS;
#endif
        }
      else if (request.command == VISION_BADGE_RPC_CAMERA)
        {
          struct vision_badge_image_s settled = {0};
          struct timespec ts;
          bool compare = !strcmp(request.prompt, "compare");
          clock_gettime(CLOCK_MONOTONIC, &ts);
          uint64_t started = (uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
          memset(&image, 0, sizeof(image));
          image.capacity = CONFIG_CONTEST2026_441_VISION_BADGE_JPEG_BUFFER_SIZE;
          if (!strcmp(request.prompt, "start") || !strcmp(request.prompt, "stop") ||
              !strcmp(request.prompt, "status"))
            {
              ret = camera_service_control(request.prompt, response.text, sizeof(response.text));
              if (ret >= 0) response.stage = VISION_BADGE_STAGE_DONE;
            }
          else if (strcmp(request.prompt, "capture") && strcmp(request.prompt, "scene") && !compare)
            ret = -EINVAL;
          else
            {
              image.data = malloc(image.capacity);
              settled.capacity = image.capacity;
              if (compare) settled.data = malloc(settled.capacity);
              response.stage = VISION_BADGE_STAGE_CAPTURE;
              feedback_service_activity(VISION_BADGE_STAGE_CAPTURE);
              ret = !image.data || (compare && !settled.data) ? -ENOMEM :
                    compare ? camera_service_capture_pair(&image, &settled) :
                              camera_service_capture(&image);
              if (ret >= 0 && (!strcmp(request.prompt, "scene") || compare))
                {
                  response.stage = VISION_BADGE_STAGE_QUERY;
                  feedback_service_activity(VISION_BADGE_STAGE_QUERY);
#ifdef CONFIG_CONTEST2026_441_AGORA
                  ret = compare ? agora_control_capture(&image, response.text,
                                                         sizeof(response.text)) :
                                  agora_control_scene(&image, response.text,
                                                       sizeof(response.text));
                  if (ret >= 0 && compare)
                    ret = agora_control_capture(&settled, response.text,
                                               sizeof(response.text));
#else
                  ret = -ENOSYS;
#endif
                }
              else if (ret >= 0)
                snprintf(response.text, sizeof(response.text),
                         "JPEG: %ux%u, %lu bytes; capture only, no upload",
                         image.width, image.height, (unsigned long)image.size);
              if (ret >= 0) response.stage = VISION_BADGE_STAGE_DONE;
              free(image.data);
              free(settled.data);
              feedback_service_activity(ret < 0 ? VISION_BADGE_STAGE_ERROR :
                                                  VISION_BADGE_STAGE_DONE);
            }
          clock_gettime(CLOCK_MONOTONIC, &ts);
          uint64_t ended = (uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
          syslog(ret < 0 ? LOG_ERR : LOG_INFO,
                 "vision_badge: camera RPC total=%lu ms ret=%d\n",
                 (unsigned long)(ended - started), ret);
        }
      else if (request.command == VISION_BADGE_RPC_BATTERY)
        {
          ret = vision_badge_battery_status(response.text,
                                            sizeof(response.text));
        }
      else if (request.command == VISION_BADGE_RPC_AUDIO_DIAGNOSTIC)
        {
          ret = vision_badge_audio_diagnostic(request.prompt, response.text,
                                              sizeof(response.text));
#if defined(CONFIG_CONTEST2026_441_POWER) && defined(CONFIG_CONTEST2026_441_AGORA)
          if (!ret && !strncmp(request.prompt, "volume ", 7))
            {
              ret = agora_control_volume_save(audio_service_volume_level());
              size_t used = strlen(response.text);
              snprintf(response.text + used, sizeof(response.text) - used,
                       "; persist=%d (RAM selection remains on write failure)", ret);
            }
#endif
        }
      else if (request.command == VISION_BADGE_RPC_CONVERSATION)
        {
#ifdef CONFIG_CONTEST2026_441_AGORA
          ret = agora_service_command(request.prompt, response.text,
                                      sizeof(response.text));
#elif defined(CONFIG_CONTEST2026_441_XIAOZHI)
          ret = xiaozhi_conversation_command(request.prompt, response.text,
                                             sizeof(response.text));
#else
          ret = -ENOSYS;
#endif
        }
      else if (request.command == VISION_BADGE_RPC_CODEC_SELFTEST)
        {
#ifdef CONFIG_CONTEST2026_441_XIAOZHI
          ret = xiaozhi_codec_selftest(response.text, sizeof(response.text));
#else
          ret = -ENOSYS;
#endif
        }
      else if (request.command == VISION_BADGE_RPC_AUDIO_LOOPBACK)
        {
          unsigned int seconds;
          unsigned int volume;
          char extra;

          if (sscanf(request.prompt, "%u %u %c", &seconds,
                     &volume, &extra) != 2)
            {
              ret = -EINVAL;
            }
          else
            {
              ret = audio_service_loopback(seconds, volume,
                                             response.text,
                                             sizeof(response.text));
            }
        }
      else if (request.command == VISION_BADGE_RPC_PROVISION)
        {
          size_t ssid_length = strnlen(request.prompt,
                                       sizeof(request.prompt));
          const char *password = ssid_length + 1 < sizeof(request.prompt) ?
                                 request.prompt + ssid_length + 1 : NULL;

          if (password == NULL ||
              memchr(password, '\0', sizeof(request.prompt) -
                                    ssid_length - 1) == NULL ||
              memchr(request.text, '\0', sizeof(request.text)) == NULL)
            {
              ret = -EINVAL;
            }
          else
            {
              ret = mimo_http_save_provision(request.prompt, password,
                                             request.text);
            }
        }
      else
        {
          memset(&context, 0, sizeof(context));
          memset(&image, 0, sizeof(image));
          memset(&result, 0, sizeof(result));
          image.capacity = CONFIG_CONTEST2026_441_VISION_BADGE_JPEG_BUFFER_SIZE;
          result.capacity = CONFIG_CONTEST2026_441_VISION_BADGE_RESULT_BUFFER_SIZE;
          image.data = malloc(image.capacity);
          result.text = malloc(result.capacity);

          if (image.data == NULL || result.text == NULL)
            {
              ret = -ENOMEM;
            }
          else
            {
              ret = vision_badge_run_once(&context, request.prompt,
                                          &image, &result);
            }

          response.stage = context.stage;
          if (ret >= 0 && result.text != NULL)
            {
              bool truncated = vision_badge_rpc_copy_result(
                response.text, sizeof(response.text), result.text,
                result.length);

              if (truncated)
                {
                  syslog(LOG_WARNING,
                         "VISION BADGE RPC answer truncated from %lu bytes\n",
                         (unsigned long)result.length);
                }
            }

          free(image.data);
          free(result.text);
        }

#ifdef CONFIG_CONTEST2026_441_WAKE
      wake_service_rpc_end(wake_exclusive);
#endif
      response.status = ret;
      memset(&request, 0, sizeof(request));
      syslog(ret < 0 ? LOG_ERR : LOG_INFO,
             "VISION BADGE RPC sequence=%lu stage=%d status=%d\n",
             (unsigned long)response.sequence, (int)response.stage, ret);
      (void)vision_badge_rpc_send(&response, VISION_BADGE_RPC_SEND_MS);
    }

  return 0;
}
#endif

#if !defined(CONFIG_BK7258_AP_CORE) && defined(CONFIG_BK7258_AIDK_BUTTONS)
static volatile bool g_vision_badge_button_worker_started;

#ifdef CONFIG_CONTEST2026_441_POWER
static unsigned int g_key_pending;
static bool g_key_busy;
static sem_t g_key_wake = SEM_INITIALIZER(0);
static int g_key_action_pid = -1;

static void vision_badge_button_queue(unsigned int action)
{
  unsigned int expected = 0;
  if (action == 4u)
    __atomic_store_n(&g_key_pending, action, __ATOMIC_RELEASE);
  else if (__atomic_load_n(&g_key_busy, __ATOMIC_ACQUIRE) ||
           !__atomic_compare_exchange_n(&g_key_pending, &expected, action, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return; /* No stale photo/voice replay while a previous RPC is in flight. */
  nxsem_post(&g_key_wake);
}

static int vision_badge_button_action_worker(int argc, char *argv[])
{
  (void)argc; (void)argv;
  for (;;)
    {
      (void)nxsem_wait_uninterruptible(&g_key_wake);
      __atomic_store_n(&g_key_busy, true, __ATOMIC_RELEASE);
      unsigned int action = __atomic_exchange_n(&g_key_pending, 0u, __ATOMIC_ACQ_REL);
      if (action)
        {
          struct vision_badge_rpc_result_s result = {0};
          int ret = action == 4u ? vision_badge_rpc_power("toggle", &result) :
                    action == 2u ? vision_badge_rpc_conversation("toggle", &result) :
                    vision_badge_rpc_audio_diagnostic(action == 1u ? "volume down" : "volume up", &result);
          syslog(ret < 0 || result.status < 0 ? LOG_ERR : LOG_INFO,
                 "VISION BADGE key action=%u transport=%d status=%d %s\n",
                 action, ret, result.status, result.text);
        }
      __atomic_store_n(&g_key_busy, false, __ATOMIC_RELEASE);
    }
  return 0;
}
#endif

static void vision_badge_button_event(unsigned int key, unsigned long held_ms)
{
#ifdef CONFIG_CONTEST2026_441_POWER
  if (key == BUTTON_K2 && held_ms >= 3000)
    { vision_badge_button_queue(4u); return; }
  if (held_ms >= 50 && held_ms < 3000 &&
      (key == BUTTON_K1 || key == BUTTON_K2 || key == BUTTON_K3))
    vision_badge_button_queue(key == BUTTON_K1 ? 1u : key == BUTTON_K2 ? 2u : 3u);
#else
  if (held_ms < 50 || held_ms >= 3000) return;
  if (key != BUTTON_K1 && key != BUTTON_K2) return;
  struct vision_badge_rpc_result_s result;
  memset(&result, 0, sizeof(result));
  syslog(LOG_INFO, "VISION BADGE %s short press; %s\n",
         key == BUTTON_K1 ? "KEY1" : "KEY2",
         key == BUTTON_K1 ? "scene capture" : "voice toggle");
  int ret = key == BUTTON_K1 ? vision_badge_rpc_camera("scene", &result) :
                              vision_badge_rpc_conversation("toggle", &result);
  syslog(ret < 0 ? LOG_ERR : LOG_INFO,
         "VISION BADGE %s request status=%d stage=%d\n",
         key == BUTTON_K1 ? "KEY1" : "KEY2",
         ret < 0 ? ret : result.status, ret < 0 ? -1 : result.stage);
  if (ret >= 0 && result.status >= 0 && result.text[0] != '\0')
    {
      printf("vision_badge: %s\n", result.text);
      fflush(stdout);
    }
#endif
}

static int vision_badge_button_worker(int argc, char *argv[])
{
#ifdef CONFIG_CONTEST2026_441_POWER
  unsigned int active = 0;
  bool initialized = false, blocked = false, long_fired = false;
  clock_t pressed_at = 0;
  (void)argc; (void)argv;
  for (;;)
    {
      unsigned int pressed = board_buttons() & (BUTTON_K1 | BUTTON_K2 | BUTTON_K3);
      if (!initialized)
        { initialized = true; blocked = pressed != 0; }
      if (blocked)
        { if (!pressed) blocked = false; }
      else if (pressed && (pressed & (pressed - 1)))
        { blocked = true; active = 0; }
      else if (active && pressed != active)
        {
          if (!pressed && !long_fired)
            vision_badge_button_event(active, (unsigned long)TICK2MSEC(clock_systime_ticks() - pressed_at));
          else if (pressed) blocked = true;
          active = 0;
        }
      else if (!active && pressed)
        { active = pressed; pressed_at = clock_systime_ticks(); long_fired = false; }
      else if (active == BUTTON_K2 && !long_fired &&
               (unsigned long)TICK2MSEC(clock_systime_ticks() - pressed_at) >= 3000)
        { long_fired = true; vision_badge_button_event(BUTTON_K2, 3000); }
      nxsig_usleep(50000);
    }
  return 0;
#else
  unsigned int previous = 0;
  bool initialized = false;
  clock_t pressed_at[2] = {0, 0};
  const unsigned int keys[2] = {BUTTON_K1, BUTTON_K2};

  (void)argc;
  (void)argv;

  for (;;)
    {
      unsigned int pressed = board_buttons() & (BUTTON_K1 | BUTTON_K2);

      if (!initialized)
        {
          previous = pressed;
          initialized = true;
        }
      else if (pressed == (BUTTON_K1 | BUTTON_K2))
        { pressed_at[0] = pressed_at[1] = 0; }
      else
        {
          for (unsigned int i = 0; i < 2; i++)
            {
              if ((pressed & keys[i]) && !(previous & keys[i]))
                pressed_at[i] = clock_systime_ticks();
              else if (!(pressed & keys[i]) && (previous & keys[i]) && pressed_at[i])
                {
                  unsigned long held_ms = (unsigned long)
                    TICK2MSEC(clock_systime_ticks() - pressed_at[i]);
                  pressed_at[i] = 0;
                  vision_badge_button_event(keys[i], held_ms);
                }
            }
        }

      previous = pressed;
      nxsig_usleep(50000);
    }

  return 0;
#endif
}
#endif

static int vision_badge_rpc_endpoint_callback(
  struct rpmsg_endpoint *endpoint, void *data, size_t len,
  uint32_t src, void *arg)
{
  struct vision_badge_rpc_dev_s *priv = arg;
  struct vision_badge_rpc_wire_s *msg = data;
  volatile struct bk7258_rptun_control_s *control =
    bk7258_rptun_control();

  (void)endpoint;
  (void)src;
  if (!vision_badge_rpc_wire_valid(msg, len) || msg->generation == 0 ||
      msg->generation != control->generation)
    {
      return -ESTALE;
    }

#ifdef CONFIG_BK7258_AP_CORE
  if (msg->command == VISION_BADGE_RPC_QUERY ||
      msg->command == VISION_BADGE_RPC_PROVISION ||
      msg->command == VISION_BADGE_RPC_AUDIO_LOOPBACK ||
      msg->command == VISION_BADGE_RPC_CODEC_SELFTEST ||
      msg->command == VISION_BADGE_RPC_CONVERSATION ||
      msg->command == VISION_BADGE_RPC_AGORA ||
      msg->command == VISION_BADGE_RPC_BATTERY ||
      msg->command == VISION_BADGE_RPC_AUDIO_DIAGNOSTIC ||
      msg->command == VISION_BADGE_RPC_CAMERA
#ifdef CONFIG_CONTEST2026_441_POWER
      || msg->command == VISION_BADGE_RPC_POWER
#endif
      )
    {
      if (__atomic_load_n(&priv->pending, __ATOMIC_ACQUIRE))
        {
          return -EBUSY;
        }

      memcpy(&priv->request, msg, sizeof(*msg));
      priv->request.prompt[sizeof(priv->request.prompt) - 1] = '\0';
      priv->request.text[sizeof(priv->request.text) - 1] = '\0';
      __asm volatile ("dmb sy" ::: "memory");
      __atomic_store_n(&priv->pending, true, __ATOMIC_RELEASE);
      return nxsem_post(&priv->semaphore);
    }
#else
  if (msg->command == VISION_BADGE_RPC_RESULT &&
      msg->sequence == priv->waiting_sequence)
    {
      priv->reply.status = msg->status;
      priv->reply.stage = msg->stage;
      memcpy(priv->reply.text, msg->text, sizeof(priv->reply.text));
      priv->reply.text[sizeof(priv->reply.text) - 1] = '\0';
      __asm volatile ("dmb sy" ::: "memory");
      priv->reply_valid = true;
      return nxsem_post(&priv->semaphore);
    }
#endif

  return -ENOMSG;
}

static void vision_badge_rpc_device_created(struct rpmsg_device *rdev,
                                             void *arg)
{
  struct vision_badge_rpc_dev_s *priv = arg;
  const char *cpuname = rpmsg_get_cpuname(rdev);

  if (cpuname == NULL || strcmp(cpuname, VISION_BADGE_RPC_REMOTE_NAME) != 0)
    {
      return;
    }

#ifdef CONFIG_BK7258_AP_CORE
  priv->endpoint.priv = priv;
  priv->connection_error = rpmsg_create_ept(
    &priv->endpoint, rdev, VISION_BADGE_RPC_ENDPOINT,
    RPMSG_ADDR_ANY, RPMSG_ADDR_ANY, vision_badge_rpc_endpoint_callback, NULL);
  if (priv->connection_error >= 0)
    {
      __atomic_store_n(&priv->endpoint_created, true, __ATOMIC_RELEASE);
    }
#else
  priv->connection_error = 0;
#endif
}

#ifndef CONFIG_BK7258_AP_CORE
static bool vision_badge_rpc_ns_match(struct rpmsg_device *rdev, void *arg,
                                      const char *name, uint32_t dest)
{
  const char *cpuname = rpmsg_get_cpuname(rdev);

  (void)arg;
  (void)dest;
  return cpuname != NULL &&
         strcmp(cpuname, VISION_BADGE_RPC_REMOTE_NAME) == 0 &&
         strcmp(name, VISION_BADGE_RPC_ENDPOINT) == 0;
}

static void vision_badge_rpc_ns_bind(struct rpmsg_device *rdev, void *arg,
                                     const char *name, uint32_t dest)
{
  struct vision_badge_rpc_dev_s *priv = arg;

  priv->endpoint.priv = priv;
  priv->connection_error = rpmsg_create_ept(
    &priv->endpoint, rdev, name, RPMSG_ADDR_ANY, dest,
    vision_badge_rpc_endpoint_callback, NULL);
  if (priv->connection_error >= 0)
    {
      __atomic_store_n(&priv->endpoint_created, true, __ATOMIC_RELEASE);
    }
}
#endif

static void vision_badge_rpc_device_destroy(struct rpmsg_device *rdev,
                                             void *arg)
{
  struct vision_badge_rpc_dev_s *priv = arg;
  const char *cpuname = rpmsg_get_cpuname(rdev);

  if (cpuname == NULL || strcmp(cpuname, VISION_BADGE_RPC_REMOTE_NAME) != 0)
    {
      return;
    }

  __atomic_store_n(&priv->endpoint_created, false, __ATOMIC_RELEASE);
  priv->connection_error = -ENOTCONN;
#ifndef CONFIG_BK7258_AP_CORE
  priv->reply_valid = false;
  (void)nxsem_post(&priv->semaphore);
#endif
  if (priv->endpoint.rdev != NULL)
    {
      rpmsg_destroy_ept(&priv->endpoint);
    }
}

int vision_badge_rpc_initialize(void)
{
  struct vision_badge_rpc_dev_s *priv = &g_vision_badge_rpc;
  bool expected = false;
  bool callback_registered = false;
  bool semaphore_initialized = false;
  int ret;

  if (!__atomic_compare_exchange_n(&priv->initialized, &expected, true,
                                   false, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE))
    {
      return 0;
    }

  ret = nxsem_init(&priv->semaphore, 0, 0);
  if (ret >= 0)
    {
      semaphore_initialized = true;
    }

  if (ret >= 0)
    {
      ret = rpmsg_register_callback(
        priv, vision_badge_rpc_device_created,
        vision_badge_rpc_device_destroy,
#ifdef CONFIG_BK7258_AP_CORE
        NULL, NULL);
#else
        vision_badge_rpc_ns_match, vision_badge_rpc_ns_bind);
#endif
    }

  if (ret >= 0)
    {
      callback_registered = true;
    }

#ifdef CONFIG_BK7258_AP_CORE
  if (ret >= 0)
    {
      cpu_set_t cpuset;
      int pid = kthread_create("vision-rpc", VISION_BADGE_RPC_PRIORITY,
                               VISION_BADGE_RPC_STACKSIZE,
                               vision_badge_rpc_worker, NULL);
      if (pid < 0)
        {
          ret = pid;
        }
#ifdef CONFIG_SMP
      else
        {
          CPU_ZERO(&cpuset);
          CPU_SET(0, &cpuset);
          ret = sched_setaffinity(pid, sizeof(cpuset), &cpuset);
          if (ret < 0)
            {
              kthread_delete(pid);
            }
        }
#else
      (void)cpuset;
#endif
    }

  if (ret >= 0)
    {
#ifdef CONFIG_CONTEST2026_441_POWER
      __atomic_store_n(&g_network_power_busy, true, __ATOMIC_RELEASE);
      int pid = power_service_work_allowed() ?
        kthread_create("vision-net", VISION_BADGE_NET_PRIORITY,
                       VISION_BADGE_NET_STACKSIZE, vision_badge_network_worker, NULL) : -ESHUTDOWN;
#else
      int pid = kthread_create("vision-net", VISION_BADGE_NET_PRIORITY,
                               VISION_BADGE_NET_STACKSIZE,
                               vision_badge_network_worker, NULL);
#endif
      if (pid < 0)
        {
#ifdef CONFIG_CONTEST2026_441_POWER
          __atomic_store_n(&g_network_power_busy, false, __ATOMIC_RELEASE);
#endif
          syslog(LOG_WARNING,
                 "VISION BADGE network warmup start failed: %d\n", pid);
        }
    }
#endif

  if (ret < 0)
    {
      if (callback_registered)
        {
          rpmsg_unregister_callback(
            priv, vision_badge_rpc_device_created,
            vision_badge_rpc_device_destroy,
#ifdef CONFIG_BK7258_AP_CORE
            NULL, NULL);
#else
            vision_badge_rpc_ns_match, vision_badge_rpc_ns_bind);
#endif
        }

      if (semaphore_initialized)
        {
          (void)nxsem_destroy(&priv->semaphore);
        }

      memset(&priv->endpoint, 0, sizeof(priv->endpoint));
      __atomic_store_n(&priv->endpoint_created, false, __ATOMIC_RELEASE);
      priv->connection_error = ret;
      __atomic_store_n(&priv->initialized, false, __ATOMIC_RELEASE);
    }

  return ret;
}

#if !defined(CONFIG_BK7258_AP_CORE) && defined(CONFIG_BK7258_AIDK_BUTTONS)
int vision_badge_rpc_buttons_start(void)
{
  bool expected = false;
  int pid;

  if (!__atomic_compare_exchange_n(&g_vision_badge_button_worker_started,
                                   &expected, true, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
      return 0;
    }

#ifdef CONFIG_CONTEST2026_441_POWER
  if (g_key_action_pid < 0)
    g_key_action_pid = kthread_create("vision-key-rpc", 100, 4096,
                                      vision_badge_button_action_worker, NULL);
  if (g_key_action_pid < 0)
    {
      __atomic_store_n(&g_vision_badge_button_worker_started, false, __ATOMIC_RELEASE);
      return g_key_action_pid;
    }
#endif
  pid = kthread_create("vision-key", 100, 4096,
                       vision_badge_button_worker, NULL);
  if (pid < 0)
    {
      __atomic_store_n(&g_vision_badge_button_worker_started, false,
                       __ATOMIC_RELEASE);
      return pid;
    }

  return 0;
}
#endif

#ifndef CONFIG_BK7258_AP_CORE
static int vision_badge_rpc_exchange(enum vision_badge_rpc_command_e command,
                                      const char *prompt, const char *text,
                                      unsigned int timeout_ms,
                                      struct vision_badge_rpc_result_s *result)
{
  struct vision_badge_rpc_dev_s *priv = &g_vision_badge_rpc;
  volatile struct bk7258_rptun_control_s *control =
    bk7258_rptun_control();
  struct vision_badge_rpc_wire_s request;
  clock_t started;
  clock_t limit;
  clock_t elapsed;
  int ret;

  if (prompt == NULL || prompt[0] == '\0' || result == NULL ||
      timeout_ms == 0 ||
      (command != VISION_BADGE_RPC_QUERY &&
       command != VISION_BADGE_RPC_PROVISION &&
       command != VISION_BADGE_RPC_AUDIO_LOOPBACK &&
       command != VISION_BADGE_RPC_CODEC_SELFTEST &&
       command != VISION_BADGE_RPC_CONVERSATION &&
       command != VISION_BADGE_RPC_AGORA &&
       command != VISION_BADGE_RPC_BATTERY &&
       command != VISION_BADGE_RPC_AUDIO_DIAGNOSTIC &&
       command != VISION_BADGE_RPC_CAMERA
#ifdef CONFIG_CONTEST2026_441_POWER
       && command != VISION_BADGE_RPC_POWER
#endif
       ))
    {
      return -EINVAL;
    }

  if (command != VISION_BADGE_RPC_PROVISION &&
      strlen(prompt) >= VISION_BADGE_RPC_PROMPT_SIZE)
    {
      return -E2BIG;
    }

  if (command == VISION_BADGE_RPC_AGORA && text != NULL &&
      strlen(text) >= VISION_BADGE_RPC_TEXT_SIZE)
    {
      return -E2BIG;
    }

  ret = vision_badge_rpc_initialize();
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  started = clock_systime_ticks();
  limit = MSEC2TICK(timeout_ms);
  while (!vision_badge_rpc_endpoint_ready() &&
         (clock_t)(clock_systime_ticks() - started) < limit)
    {
      nxsig_usleep(10000);
    }

  if (!vision_badge_rpc_endpoint_ready())
    {
      ret = -ENOTCONN;
      goto out;
    }

  while (nxsem_trywait(&priv->semaphore) == 0)
    {
    }

  if (++priv->next_sequence == 0)
    {
      priv->next_sequence++;
    }

  memset(&request, 0, sizeof(request));
  request.magic = VISION_BADGE_RPC_MAGIC;
  request.version = VISION_BADGE_RPC_VERSION;
  request.command = command;
  request.generation = control->generation;
  request.sequence = priv->next_sequence;
  if (command == VISION_BADGE_RPC_PROVISION)
    {
      memcpy(request.prompt, prompt, sizeof(request.prompt));
      memcpy(request.text, text, sizeof(request.text));
    }
  else
    {
      strncpy(request.prompt, prompt, sizeof(request.prompt) - 1);
      if (command == VISION_BADGE_RPC_AGORA && text != NULL)
        {
          strncpy(request.text, text, sizeof(request.text) - 1);
        }
    }
  priv->waiting_sequence = request.sequence;
  priv->reply_valid = false;

  ret = vision_badge_rpc_send(&request, VISION_BADGE_RPC_SEND_MS);
  memset(&request, 0, sizeof(request));
  if (ret < 0)
    {
      goto out;
    }

  elapsed = clock_systime_ticks() - started;
  if (elapsed >= limit)
    {
      ret = -ETIMEDOUT;
      goto out;
    }

  ret = nxsem_tickwait_uninterruptible(&priv->semaphore, limit - elapsed);
  if (ret < 0)
    {
      goto out;
    }

  __asm volatile ("dmb sy" ::: "memory");
  if (!priv->reply_valid)
    {
      ret = priv->connection_error < 0 ?
            priv->connection_error : -ESTALE;
      goto out;
    }

  memcpy(result, &priv->reply, sizeof(*result));
  ret = 0;

out:
  nxmutex_unlock(&priv->lock);
  return ret;
}

#ifdef CONFIG_CONTEST2026_441_POWER
int vision_badge_rpc_power(const char *action,
                           struct vision_badge_rpc_result_s *result)
{
  if (!action || (strcmp(action, "status") && strcmp(action, "quiet") &&
      strcmp(action, "prepare") && strcmp(action, "resume") && strcmp(action, "off") && strcmp(action, "toggle")))
    return -EINVAL;
  return vision_badge_rpc_exchange(VISION_BADGE_RPC_POWER, action, NULL, 25000, result);
}
#endif

int vision_badge_rpc_battery(struct vision_badge_rpc_result_s *result)
{
  return vision_badge_rpc_exchange(VISION_BADGE_RPC_BATTERY, "status", NULL,
                                   15000, result);
}

int vision_badge_rpc_audio_diagnostic(const char *action,
                                      struct vision_badge_rpc_result_s *result)
{
  return vision_badge_rpc_exchange(VISION_BADGE_RPC_AUDIO_DIAGNOSTIC,
                                   action, NULL, 15000, result);
}

int vision_badge_rpc_camera(const char *action,
                            struct vision_badge_rpc_result_s *result)
{
  return vision_badge_rpc_exchange(VISION_BADGE_RPC_CAMERA,
                                   action, NULL, 60000, result);
}

int vision_badge_rpc_query(const char *prompt, unsigned int timeout_ms,
                           struct vision_badge_rpc_result_s *result)
{
  return vision_badge_rpc_exchange(VISION_BADGE_RPC_QUERY, prompt, NULL,
                                   timeout_ms, result);
}

int vision_badge_rpc_conversation(const char *action,
                                  struct vision_badge_rpc_result_s *result)
{
  return vision_badge_rpc_exchange(VISION_BADGE_RPC_CONVERSATION,
                                   action, NULL, 5000, result);
}

int vision_badge_rpc_agora(const char *action,
                           struct vision_badge_rpc_result_s *result)
{
  if (action != NULL && !strncmp(action, "config ", 7))
    {
      return vision_badge_rpc_exchange(VISION_BADGE_RPC_AGORA,
                                       "config", action + 7, 15000, result);
    }

  return vision_badge_rpc_exchange(VISION_BADGE_RPC_AGORA,
                                   action, NULL, 15000, result);
}

int vision_badge_rpc_codec_selftest(struct vision_badge_rpc_result_s *result)
{
  return vision_badge_rpc_exchange(VISION_BADGE_RPC_CODEC_SELFTEST,
                                   "selftest", NULL, 30000, result);
}

int vision_badge_rpc_audio_loopback(unsigned int seconds,
                                    unsigned int volume,
                                    struct vision_badge_rpc_result_s *result)
{
  char prompt[32];

  if (seconds < 1 || seconds > 5 || volume > 100)
    {
      return -EINVAL;
    }

  snprintf(prompt, sizeof(prompt), "%u %u", seconds, volume);
  return vision_badge_rpc_exchange(VISION_BADGE_RPC_AUDIO_LOOPBACK,
                                   prompt, NULL, 30000, result);
}

int vision_badge_rpc_provision(const char *ssid, const char *password,
                               const char *api_key, unsigned int timeout_ms,
                               struct vision_badge_rpc_result_s *result)
{
  char prompt[VISION_BADGE_RPC_PROMPT_SIZE] = {0};
  char key[VISION_BADGE_RPC_TEXT_SIZE] = {0};
  size_t ssid_length;
  size_t password_length;
  size_t key_length;
  int ret;

  if (ssid == NULL || password == NULL || api_key == NULL)
    {
      return -EINVAL;
    }

  ssid_length = strlen(ssid);
  password_length = strlen(password);
  key_length = strlen(api_key);
  if (ssid_length == 0 || ssid_length > 32 || password_length > 63 ||
      key_length == 0 || key_length >= sizeof(key) ||
      ssid_length + password_length + 2 > sizeof(prompt))
    {
      return -EINVAL;
    }

  memcpy(prompt, ssid, ssid_length);
  memcpy(prompt + ssid_length + 1, password, password_length);
  memcpy(key, api_key, key_length);
  ret = vision_badge_rpc_exchange(VISION_BADGE_RPC_PROVISION,
                                   prompt, key, timeout_ms, result);
  memset(prompt, 0, sizeof(prompt));
  memset(key, 0, sizeof(key));
  return ret;
}
#endif
