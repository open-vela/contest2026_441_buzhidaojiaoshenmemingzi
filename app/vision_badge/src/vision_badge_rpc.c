/* SPDX-License-Identifier: Apache-2.0 */

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/rpmsg/rpmsg.h>
#include <nuttx/semaphore.h>
#include <nuttx/signal.h>

#include <arch/chip/bk7258_rptun.h>

#include <vision_badge/rpc.h>

#ifdef CONFIG_BK7258_AP_CORE
#  include <sched.h>
#  include <stdlib.h>
#  include <syslog.h>
#  include <nuttx/kthread.h>
#  include <vision_badge/config.h>
#  include <vision_badge/mimo_http.h>
#  include <vision_badge/services.h>
#  include <vision_badge/workflow.h>
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
#  define VISION_BADGE_RPC_STACKSIZE    16384
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
  VISION_BADGE_RPC_PROVISION
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
          msg->command == VISION_BADGE_RPC_PROVISION);
}

#ifdef CONFIG_BK7258_AP_CORE
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
      ret = mimo_http_prepare_session();
      if (ret == 0)
        {
          syslog(LOG_INFO, "VISION BADGE network session ready\n");
          return 0;
        }

      syslog(LOG_WARNING,
             "VISION BADGE network warmup attempt=%u ret=%d\n",
             attempt + 1, ret);
      nxsig_usleep(2000000);
    }

  syslog(LOG_ERR, "VISION BADGE network warmup failed\n");
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

      if (request.command == VISION_BADGE_RPC_PROVISION)
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

static int vision_badge_button_worker(int argc, char *argv[])
{
  const char prompt[] = "请简短描述画面中能确认的内容";
  bool previous = false;
  bool initialized = false;
  clock_t pressed_at = 0;

  (void)argc;
  (void)argv;

  for (;;)
    {
      bool pressed = (board_buttons() & BUTTON_K1) != 0;

      if (!initialized)
        {
          previous = pressed;
          initialized = true;
        }
      else if (pressed && !previous)
        {
          pressed_at = clock_systime_ticks();
        }
      else if (!pressed && previous && pressed_at != 0)
        {
          unsigned long held_ms =
            (unsigned long)TICK2MSEC(clock_systime_ticks() - pressed_at);

          if (held_ms >= 50 && held_ms < 3000)
            {
              struct vision_badge_rpc_result_s result;
              int ret;

              memset(&result, 0, sizeof(result));
              syslog(LOG_INFO,
                     "VISION BADGE KEY1 short press; starting capture\n");
              ret = vision_badge_rpc_query(prompt, 120000, &result);
              syslog(ret < 0 ? LOG_ERR : LOG_INFO,
                     "VISION BADGE KEY1 request status=%d stage=%d\n",
                     ret < 0 ? ret : result.status,
                     ret < 0 ? -1 : result.stage);
              if (ret >= 0 && result.status >= 0 && result.text[0] != '\0')
                {
                  printf("vision_badge: %s\n", result.text);
                  fflush(stdout);
                }
            }

          pressed_at = 0;
        }

      previous = pressed;
      nxsig_usleep(50000);
    }

  return 0;
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
      msg->command == VISION_BADGE_RPC_PROVISION)
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
      int pid = kthread_create("vision-net", VISION_BADGE_NET_PRIORITY,
                               VISION_BADGE_NET_STACKSIZE,
                               vision_badge_network_worker, NULL);
      if (pid < 0)
        {
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
       command != VISION_BADGE_RPC_PROVISION))
    {
      return -EINVAL;
    }

  if (command == VISION_BADGE_RPC_QUERY &&
      strlen(prompt) >= VISION_BADGE_RPC_PROMPT_SIZE)
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

int vision_badge_rpc_query(const char *prompt, unsigned int timeout_ms,
                           struct vision_badge_rpc_result_s *result)
{
  return vision_badge_rpc_exchange(VISION_BADGE_RPC_QUERY, prompt, NULL,
                                   timeout_ms, result);
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
