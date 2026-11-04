/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "chips/bk7258/cp/bk7258_pm_soft_off.c"
#include "chips/bk7258/cp/bk7258_storage.c"

static jmp_buf reset_jump;
static unsigned int reason, reset_reason, consumed;
static int queue_count, queue_error, guard_error, init_count, sleep_count, sync_count;
static int gpio_error, wake_error, mode_count;
static bool high = true;
static unsigned int reads, release_after, delays;
static clock_t now;
static unsigned int wait_ms=20, release_ms, fail_read_ms;
static void (*queued)(void *);
static bool interrupt_context;
static pid_t current_tid=17;
bool up_interrupt_context(void) { return interrupt_context; }
pid_t nxsched_gettid(void) { return current_tid; }
int nxmutex_lock(mutex_t *lock) { if(*lock)return -EBUSY;*lock=1;return 0; }
int nxmutex_timedlock(mutex_t *lock,unsigned int timeout) {
  assert(timeout==1000);if(guard_error)return guard_error;
  if(*lock)return -ETIMEDOUT;
  *lock=1;return 0;
}
int nxmutex_unlock(mutex_t *lock) { assert(*lock);*lock=0;return 0; }
#ifdef CONFIG_BK7258_MCUBOOT_IMAGE
static bool confirmed = true;
int bk7258_ota_get_active_pair(struct bk7258_ota_pair_snapshot_s *pair)
{ pair->state = confirmed ? BK7258_OTA_PAIR_CONFIRMED : BK7258_OTA_PAIR_PENDING; return 0; }
#endif
int work_queue(int id, struct work_s *work, void (*callback)(void *), void *arg, unsigned int delay)
{
  assert(id == LPWORK && work && !arg && delay == 4000);
  queue_count++; queued = callback; return queue_error;
}
void sync(void) { sync_count++; }
void bk7258_system_reset(enum bk7258_reset_source_e source)
{ reset_reason = source; longjmp(reset_jump, 1); }
int bk7258_reset_cause_read(struct bk7258_reset_cause_raw_s *raw) { raw->source = reason; return 0; }
void bk_misc_set_reset_reason(uint32_t type) { assert(type == BK7258_RESET_SOURCE_REBOOT); consumed++; }
int bk7258_gpio_configure_input(uint8_t pin, enum bk7258_gpio_pull_e pull)
{ assert(consumed && pin == 12 && pull == BK7258_GPIO_PULL_UP); return gpio_error; }
int bk7258_gpio_read_input(uint8_t pin, bool *value) {
  assert(pin == 12);reads++;
  if(fail_read_ms && now>=fail_read_ms)return -EIO;
  /* Shutdown wake may retain a low GPIO state until PM hardware recovery.
   * This models the ordering contract, not proof of the actual R1 cause. */
  if(!init_count)*value=false;
  else if((release_ms && now>=release_ms) || (release_after && reads>release_after))*value=true;
  else *value=high;
  return 0;
}
void pm_hardware_init(void) { assert(consumed); init_count++; }
int bk_gpio_ana_register_wakeup_source(unsigned int pin, int type)
{ assert(init_count && pin == 12 && type == GPIO_INT_TYPE_LOW_LEVEL); return wake_error; }
int bk_pm_sleep_mode_set(int mode) { assert(mode == 3); mode_count++; return 0; }
void up_mdelay(unsigned int ms) { assert(ms == 20); delays+=ms; }
clock_t clock_systime_ticks(void) { return now; }
int nxsig_usleep(useconds_t us) { assert(us==20000);now+=wait_ms;return wait_ms<20 ? -EINTR : 0; }
irqstate_t up_irq_save(void) { return 0; }
void bk_pm_enter_sleep(void) { assert(mode_count); sleep_count++; }

int main(int argc, char **argv)
{
  assert(argc == 2);
  /* Same storage topology as the R1 product: SD-NAND data, no on-chip DATA
   * filesystem. Exercise the real guard, not an always-success substitute. */
  struct bk7258_radio_storage_config_s radio={.version=BK7258_RADIO_STORAGE_CONFIG_VERSION,
    .size=sizeof(radio),.backup={.start=0x7fe000,.size=4096},.network={.start=0x7ff000,.size=4096}};
  struct bk7258_storage_config_s storage={.version=BK7258_STORAGE_CONFIG_VERSION,
    .size=sizeof(storage),.data_storage=NULL,.radio_storage=&radio};
  assert(bk7258_storage_guard_lock(BK7258_STORAGE_GUARD_QUIESCE,false,1000)==-EAGAIN);
  assert(bk7258_storage_configure(&storage)==0);
  if(!strcmp(argv[1],"storage-barrier")) {
    assert(bk7258_storage_guard_lock(BK7258_STORAGE_GUARD_DATA,false,1000)==-EINVAL);
    assert(bk7258_storage_guard_lock(BK7258_STORAGE_GUARD_QUIESCE,true,1000)==-EPERM);
    assert(!g_bk7258_storage_guard_depth);
    assert(!bk7258_storage_guard_lock(BK7258_STORAGE_GUARD_QUIESCE,false,1000));
    assert(!bk7258_storage_guard_lock(BK7258_STORAGE_GUARD_QUIESCE,false,1000));
    assert(g_bk7258_storage_guard_depth==2);
    assert(!bk7258_storage_guard_write_authorized(0x7fe000,4));
    assert(bk7258_storage_guard_lock(BK7258_STORAGE_GUARD_RADIO,true,1000)==-EDEADLK);
    bk7258_storage_guard_unlock();assert(g_bk7258_storage_guard_depth==1);
    current_tid=18;
    assert(bk7258_storage_guard_lock(BK7258_STORAGE_GUARD_RADIO,true,1000)==-ETIMEDOUT);
    current_tid=17;bk7258_storage_guard_unlock();
    assert(!bk7258_storage_guard_lock(BK7258_STORAGE_GUARD_RADIO,true,1000));
    assert(bk7258_storage_guard_write_authorized(0x7fe000,4));
    assert(!bk7258_storage_guard_write_authorized(0x600000,4));
    bk7258_storage_guard_unlock();interrupt_context=true;
    assert(bk7258_storage_guard_lock(BK7258_STORAGE_GUARD_QUIESCE,false,1000)==-EINVAL);
    puts("CP_SOFT_OFF_STORAGE_BARRIER_PASS actual storage guard/no DATA/read-only/nesting/other-owner contention/write-scope/IRQ refusal");return 0;
  }
  struct bk7258_gpio_config_s config = {.power_button_enabled=true,
    .power_button_gpio=12, .power_button_active_low=true};
  if (!strcmp(argv[1], "unready"))
    { assert(bk7258_pm_soft_off_request() == -ENOTSUP && !queue_count); }
  else if (!strcmp(argv[1], "request") || !strcmp(argv[1], "queue-fail") ||
           !strcmp(argv[1], "storage-busy") || !strcmp(argv[1], "trial"))
    {
      bk7258_pm_soft_off_boot(&config);
      if (!strcmp(argv[1], "trial"))
        {
#ifdef CONFIG_BK7258_MCUBOOT_IMAGE
          confirmed = false;
          assert(bk7258_pm_soft_off_request() == -EBUSY && !queue_count);
          confirmed = true;
#endif
        }
      if (!strcmp(argv[1], "queue-fail")) queue_error = -EBUSY;
      assert(bk7258_pm_soft_off_request() == queue_error && queue_count == 1);
      if (queue_error) assert(!bk7258_pm_soft_off_status());
      else
        {
          assert(bk7258_pm_soft_off_status() == 1);
          assert(bk7258_pm_soft_off_request() == 0 && queue_count == 1);
          if (!strcmp(argv[1], "storage-busy"))
            { guard_error = -EBUSY; queued(NULL); assert(!bk7258_pm_soft_off_status() && !reset_reason); }
          else
            { if (!setjmp(reset_jump)) queued(NULL); assert(reset_reason == BK7258_RESET_SOURCE_FORCE_DEEPSLEEP);
              assert(g_bk7258_storage_guard_depth==1 && !g_bk7258_storage_guard_write &&
                     g_bk7258_storage_guard_owner==BK7258_STORAGE_GUARD_QUIESCE);
              assert(!bk7258_storage_guard_write_authorized(0x7fe000,4)); }
        }
    }
  else
    {
      reason = BK7258_RESET_SOURCE_FORCE_DEEPSLEEP;
      if (!strcmp(argv[1], "held-key")) high = false;
      else if (!strcmp(argv[1], "release-delay")) {high=false;release_after=10;}
      else if (!strcmp(argv[1], "wake-hold") || !strcmp(argv[1], "wake-short"))
        {reason=BK7258_RESET_SOURCE_SUPER_DEEP;high=false;if(!strcmp(argv[1],"wake-short"))release_after=10;}
      else if (!strcmp(argv[1], "wake-gpio-hold"))
        {reason=BK7258_RESET_SOURCE_DEEPPS_GPIO;high=false;}
      else if (!strcmp(argv[1], "wake-fast-waits"))
        {reason=BK7258_RESET_SOURCE_SUPER_DEEP;high=false;wait_ms=5;release_ms=500;}
      else if (!strcmp(argv[1], "wake-slow-waits"))
        {reason=BK7258_RESET_SOURCE_SUPER_DEEP;high=false;wait_ms=80;}
      else if (!strcmp(argv[1], "wake-late-release"))
        {reason=BK7258_RESET_SOURCE_SUPER_DEEP;high=false;release_ms=2990;}
      else if (!strcmp(argv[1], "wake-released"))
        {reason=BK7258_RESET_SOURCE_SUPER_DEEP;}
      else if (!strcmp(argv[1], "wake-read-fail"))
        {reason=BK7258_RESET_SOURCE_SUPER_DEEP;high=false;fail_read_ms=100;}
      else if (!strcmp(argv[1], "bad-pin")) config.power_button_gpio = 16;
      else if (!strcmp(argv[1], "gpio-fail")) gpio_error = -EIO;
      else if (!strcmp(argv[1], "wake-fail")) wake_error = -EIO;
      else assert(!strcmp(argv[1], "sleep-return"));
      if (!setjmp(reset_jump)) bk7258_pm_soft_off_boot(&config);
      assert(consumed == 1 && !queue_count);
      if (!strcmp(argv[1], "wake-hold") || !strcmp(argv[1], "wake-gpio-hold"))
        {assert(init_count==1 && !reset_reason && !sleep_count && now>=3000 && !delays);}
      else if (!strcmp(argv[1], "wake-slow-waits"))
        {assert(init_count==1 && !reset_reason && !sleep_count && now>=3000);}
      else if (!strcmp(argv[1], "wake-fast-waits") || !strcmp(argv[1], "wake-late-release"))
        {assert(init_count==1 && sleep_count==1 && now>=release_ms+100 && now<release_ms+150);}
      else if (!strcmp(argv[1], "wake-read-fail"))
        {assert(init_count==1 && !sleep_count && !reset_reason && now==100);}
      else if (!strcmp(argv[1], "held-key") || !strcmp(argv[1], "bad-pin") || !strcmp(argv[1], "gpio-fail"))
        assert(init_count==(!strcmp(argv[1],"bad-pin") ? 0 : 1) && !reset_reason && !sleep_count);
      else
        {
          assert(init_count == 1 && reset_reason == BK7258_RESET_SOURCE_REBOOT);
          assert(sleep_count == (wake_error ? 0 : 1));
        }
    }
  printf("CP_SOFT_OFF_PASS %s (reset/SDK/Flash mocked)\n", argv[1]);
  return 0;
}
