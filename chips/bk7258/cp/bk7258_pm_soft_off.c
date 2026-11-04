/****************************************************************************
 * chips/bk7258/cp/bk7258_pm_soft_off.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Explicit whole-chip reboot-to-sleep. Never stop a live AP alone: the CP
 * Wi-Fi controller retains AP-owned buffers until a whole-chip reset.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/signal.h>
#include <nuttx/wqueue.h>

#include <arch/chip/bk7258_gpio.h>
#include <arch/chip/bk7258_pinmux.h>
#include <arch/chip/bk7258_pm.h>
#ifdef CONFIG_BK7258_MCUBOOT_IMAGE
#  include <arch/chip/bk7258_ota.h>
#endif
#include <arch/chip/bk7258_reset_cause.h>
#include <arch/chip/bk7258_storage_guard.h>
#include <arch/chip/bk7258_system_reset.h>

#include <driver/gpio.h>
/* pm.h in the reduced archive bundle depends on an omitted sys_types.h.
 * Keep its pinned C enum ABI private, as the existing PM server does.
 */

#define BK7258_SDK_PM_SUPER_DEEP_SLEEP 3
extern int bk_pm_sleep_mode_set(int mode);
extern void bk_pm_enter_sleep(void);

/* Exported by the pinned cp-aidk archives; reduced headers omit these. */

extern void pm_hardware_init(void);
extern void bk_misc_set_reset_reason(uint32_t type);

static struct work_s g_soft_off_work;
static bool g_soft_off_ready;
static bool g_soft_off_pending;

static void bk7258_pm_soft_off_worker(void *arg)
{
  int ret;

  (void)arg;
  sync();

  /* Serialize reset with every CP Flash transaction, including OTA and radio
   * persistence. The AP caller has already drained and synced its owners.
   * Hold this read-only guard until reset, so no new Flash mutation can race.
   */

  ret = bk7258_storage_guard_lock(BK7258_STORAGE_GUARD_QUIESCE, false, 1000);
  if (ret < 0)
    {
      syslog(LOG_ERR, "soft-off: storage barrier rejected ret=%d; reset canceled\n", ret);
      __atomic_store_n(&g_soft_off_pending, false, __ATOMIC_RELEASE);
      return;
    }

  bk7258_system_reset(BK7258_RESET_SOURCE_FORCE_DEEPSLEEP);
}

int bk7258_pm_soft_off_status(void)
{
  return __atomic_load_n(&g_soft_off_pending, __ATOMIC_ACQUIRE) ? 1 : 0;
}

int bk7258_pm_soft_off_request(void)
{
#ifdef CONFIG_BK7258_MCUBOOT_IMAGE
  struct bk7258_ota_pair_snapshot_s pair;
#endif
  bool expected = false;
  int ret;

  if (!g_soft_off_ready)
    {
      return -ENOTSUP;
    }

  /* Do not turn a trial-image power request into an unintended rollback. */

#ifdef CONFIG_BK7258_MCUBOOT_IMAGE
  ret = bk7258_ota_get_active_pair(&pair);
  if (ret < 0 || pair.state != BK7258_OTA_PAIR_CONFIRMED)
    {
      return -EBUSY;
    }
#endif

  if (!__atomic_compare_exchange_n(&g_soft_off_pending, &expected, true,
                                   false, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE))
    {
      return OK;
    }

  /* Reply/replay has time to finish before reset; never block the RPMsg RX
   * worker with filesystem work. Duplicate requests do not schedule resets.
   */

  ret = work_queue(LPWORK, &g_soft_off_work, bk7258_pm_soft_off_worker,
                   NULL, MSEC2TICK(4000));
  if (ret < 0)
    {
      __atomic_store_n(&g_soft_off_pending, false, __ATOMIC_RELEASE);
    }

  return ret;
}

void bk7258_pm_soft_off_boot(const struct bk7258_gpio_config_s *config)
{
  struct bk7258_reset_cause_raw_s reason;
  bool high;
  bool wake;
  int ret;

  g_soft_off_ready = config != NULL && config->power_button_enabled &&
                     config->power_button_gpio <= 15;
  if (bk7258_reset_cause_read(&reason) < 0)
    {
      return;
    }
  wake = reason.source == BK7258_RESET_SOURCE_DEEPPS_GPIO ||
         reason.source == BK7258_RESET_SOURCE_SUPER_DEEP;
  if (reason.source != BK7258_RESET_SOURCE_FORCE_DEEPSLEEP && !wake) return;

  /* Consume the intent before any fallible setup. A failed entry must boot
   * normally, and a later analog-key wake must not repeat the off request.
   */

  bk_misc_set_reset_reason(BK7258_RESET_SOURCE_REBOOT);
  if (!g_soft_off_ready)
    {
      return;
    }

  /* Match the SDK's shutdown-wake ordering: low-power hardware recovery
   * (including AON GPIO state unlock) precedes reading the power key. The
   * coordinated-standby early hook invokes only the hardware leaf; complete
   * the SDK PM setup here, once, before GPIO setup and either key wait.
   * This special boot still has no radio/AP/SD owners.
   */

  pm_hardware_init();

  ret = bk7258_gpio_configure_input(config->power_button_gpio,
    config->power_button_active_low ? BK7258_GPIO_PULL_UP :
                                      BK7258_GPIO_PULL_DOWN);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "soft-off: wake key setup failed; booting\n");
      return;
    }

  if (wake)
    {
      /* Analog GPIO wakes the SoC on the first edge. Require the remaining
       * physical hold here, before starting radio/AP/SD/product workers. A
       * short tap returns to the same SDK sleep path, without a full boot. */
      clock_t started = clock_systime_ticks();
      clock_t held = 0;
      syslog(LOG_INFO, "soft-off: checking physical power key after PM recovery\n");
      for (;;)
        {
          ret = bk7258_gpio_read_input(config->power_button_gpio, &high);
          if (ret < 0 || high == config->power_button_active_low) break;
          held = clock_systime_ticks() - started;
          if (held >= MSEC2TICK(3000))
            {
              syslog(LOG_INFO, "soft-off: power key held 3000 ms; boot accepted\n");
              return;
            }
          /* CPU-loop calibration changes with DVFS. Measure elapsed NuttX
           * time; a short/interrupted wait must never count as a full poll.
           */

          (void)nxsig_usleep(20000);
        }
      syslog(LOG_INFO, "soft-off: short wake rejected after %lu ms read=%d\n",
             (unsigned long)TICK2MSEC(clock_systime_ticks() - started), ret);
    }

  /* Long-off is dispatched while the key is still down. Wait for release
   * before arming level-sensitive wake, so this same hold cannot boot again.
   * Bound a stuck input; never claim sleep if the key cannot be released. */
  clock_t started = clock_systime_ticks();
  clock_t released_at = started;
  bool released = false;
  while (clock_systime_ticks() - started < MSEC2TICK(10000))
    {
      ret = bk7258_gpio_read_input(config->power_button_gpio, &high);
      if (ret < 0) break;
      if (high != config->power_button_active_low)
        {
          released = false;
        }
      else if (!released)
        {
          released = true;
          released_at = clock_systime_ticks();
        }
      else if (clock_systime_ticks() - released_at >= MSEC2TICK(100))
        {
          break;
        }

      (void)nxsig_usleep(20000);
    }
  if (ret < 0 || !released ||
      clock_systime_ticks() - released_at < MSEC2TICK(100))
    {
      syslog(LOG_ERR, "soft-off: key stuck/read failed; sleep canceled\n");
      return;
    }

  /* This hook runs before radio/AP/SD startup. Use the SDK's complete sleep
   * setup only in this one-shot boot path; ordinary NuttX idle stays owned by
   * the existing PM policy. GPIO16 (SD D0) is changed by the analog callback,
   * so this must never be called from a running product-key callback.
   */

  ret = bk_gpio_ana_register_wakeup_source(config->power_button_gpio,
    config->power_button_active_low ? GPIO_INT_TYPE_LOW_LEVEL :
                                      GPIO_INT_TYPE_HIGH_LEVEL);
  if (ret == BK_OK)
    {
      ret = bk_pm_sleep_mode_set(BK7258_SDK_PM_SUPER_DEEP_SLEEP);
    }

  if (ret == BK_OK)
    {
      syslog(LOG_INFO, "soft-off: entering super-deep; wake gpio=%u\n",
             config->power_button_gpio);
      up_mdelay(20);
      (void)up_irq_save();
      bk_pm_enter_sleep();
    }

  /* SDK entry may abort on a pending interrupt or a power-domain vote.
   * GPIO/clock setup may already have changed: cold boot, never resume the
   * partially quiesced system and never leave the sleep-intent latch set.
   */

  bk7258_system_reset(BK7258_RESET_SOURCE_REBOOT);
}
