/* SPDX-License-Identifier: Apache-2.0 */
/* R1 graceful product power boundary. No automatic battery cutoff, forced
 * reset, direct GPIO/PMU writes or RTC/cloud startup. */
#include <nuttx/config.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/signal.h>
#include <arch/board/board.h>
#include <arch/chip/bk7258_pm.h>
#include <vision_badge/agora_service.h>
#include <vision_badge/power_service.h>
#include <vision_badge/services.h>
#include <vision_badge/rpc.h>

enum power_state_e { ACTIVE, DRAINING, QUIET, PENDING, UNKNOWN, FAILED };
static const char *const names[] = {"active", "draining", "quiet", "pending", "unknown", "failed"};
static unsigned int g_state;
static int g_error;
static bool g_camera_owned;
static bool g_display_owned;
/* A failed camera drain/restart may retain DMA or lose pause ownership.
 * Keep admission closed until reboot rather than guessing its state. */
static int g_camera_fault;
static mutex_t g_power_lock = NXMUTEX_INITIALIZER;

static void publish(enum power_state_e state, int error)
{
  __atomic_store_n(&g_error, error, __ATOMIC_RELEASE);
  __atomic_store_n(&g_state, state, __ATOMIC_RELEASE);
}

bool power_service_work_allowed(void)
{
  return __atomic_load_n(&g_state, __ATOMIC_ACQUIRE) == ACTIVE;
}

static int drain(void)
{
  char detail[192];
  clock_t started = clock_systime_ticks();
  int ret;
  publish(DRAINING, 0);
  do
    {
      ret = agora_service_power_quiesce();
      if (!ret && !vision_badge_power_network_idle()) ret = -EAGAIN;
      if (ret != -EAGAIN) break;
      if ((clock_t)(clock_systime_ticks() - started) >= MSEC2TICK(15000))
        { ret = -ETIMEDOUT; break; }
      nxsig_usleep(10000);
    }
  while (true);
  if (ret < 0) return ret;
  ret = camera_service_control("pause", detail, sizeof(detail));
  if (ret < 0) { g_camera_fault = ret; return ret; }
  g_camera_owned = true;
  /* Record intent before waiting: a timed-out display request may still be
   * applied by its owner. Resume must explicitly acknowledge the inverse. */
  g_display_owned = true;
  ret = bk7258_aidk_eye_power(false);
  if (ret < 0) return ret;
  publish(QUIET, 0);
  return 0;
}

static int resume(void)
{
  char detail[192];
  if (g_camera_fault) return g_camera_fault;
  int ret = agora_service_power_quiesce();
  if (ret < 0) return ret;
  if (g_display_owned)
    {
      ret = bk7258_aidk_eye_power(true);
      if (ret < 0) return ret;
      g_display_owned = false;
    }
  if (g_camera_owned)
    {
      ret = camera_service_control("resume", detail, sizeof(detail));
      if (ret < 0) { g_camera_fault = ret; return ret; }
      g_camera_owned = false;
    }
  ret = agora_service_power_resume();
  if (ret < 0) return ret;
  publish(ACTIVE, 0);
  return 0;
}

int power_service_command(const char *action, char *text, size_t capacity)
{
  if (!action || !text || !capacity) return -EINVAL;
  if (strcmp(action, "status") && strcmp(action, "quiet") &&
      strcmp(action, "prepare") && strcmp(action, "resume") && strcmp(action, "off") && strcmp(action, "toggle"))
    return -EINVAL;
  int ret = nxmutex_trylock(&g_power_lock);
  if (ret < 0)
    {
      snprintf(text, capacity, "power: busy state=%s", names[__atomic_load_n(&g_state, __ATOMIC_ACQUIRE)]);
      return ret;
    }
  unsigned int state = __atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
  if (state == PENDING || state == UNKNOWN)
    {
      int pending = bk7258_pm_soft_off_status();
      if (pending == 0) { publish(FAILED, -ECANCELED); state = FAILED; }
      else if (pending < 0) { publish(UNKNOWN, pending); state = UNKNOWN; }
    }
  if (!strcmp(action, "toggle")) action = state == ACTIVE ? "off" : "resume";
  if (!strcmp(action, "status")) ret = 0;
  else if (state == PENDING || state == UNKNOWN) ret = -EINPROGRESS;
  else if (!strcmp(action, "resume"))
    {
      ret = state == ACTIVE ? 0 : resume();
      if (ret < 0) publish(FAILED, ret);
    }
  else if (state == FAILED) ret = __atomic_load_n(&g_error, __ATOMIC_ACQUIRE);
  else
    {
      ret = state == ACTIVE ? drain() : 0;
      if (ret < 0) publish(FAILED, ret);
      else if (!strcmp(action, "prepare") || !strcmp(action, "off"))
        {
          /* Owners are stopped; sync() is requested, not proof of physical
           * SD-NAND durability or measured low power. CP guards Flash again. */
          sync();
          if (!strcmp(action, "off"))
            {
              ret = bk7258_pm_soft_off_request();
              if (!ret) publish(PENDING, 0);
              else
                {
                  int pending = bk7258_pm_soft_off_status();
                  if (pending > 0) publish(PENDING, ret);
                  else if (pending < 0) publish(UNKNOWN, ret);
                  else publish(FAILED, ret);
                }
            }
        }
    }
  state = __atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
  snprintf(text, capacity, "power: state=%s error=%d camera_owned=%d display_owned=%d reboot_required=%d; acceptance is not sleep/current proof",
           names[state], __atomic_load_n(&g_error, __ATOMIC_ACQUIRE), g_camera_owned, g_display_owned, g_camera_fault != 0);
  nxmutex_unlock(&g_power_lock);
  return ret;
}
