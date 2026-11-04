/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vision_badge/power_service.h>

static clock_t ticks;
static int voice_busy, voice_error, camera_error, camera_resume_error, display_error, resume_error;
static bool network_busy, inhibited, camera_owned, display_enabled = true;
static int pause_calls, display_calls, sync_calls, off_calls, off_error, pending = 1;
static int waits;
clock_t clock_systime_ticks(void) { return ticks; }
int nxsig_usleep(unsigned int delay) { ticks += delay / 1000; waits++; return 0; }
int nxmutex_trylock(int *lock) { if (*lock) return -EBUSY; *lock = 1; return 0; }
void nxmutex_unlock(int *lock) { assert(*lock); *lock = 0; }
int agora_service_power_quiesce(void)
{
  char text[192];
  inhibited = true;
  assert(!power_service_work_allowed());
  /* Reentry must return busy; it must not run owners or release the lock. */
  assert(power_service_command("resume", text, sizeof(text)) == -EBUSY);
  if (voice_error) return voice_error;
  if (voice_busy > 0) { voice_busy--; return -EAGAIN; }
  return 0;
}
int agora_service_power_resume(void) { if (resume_error) return resume_error; inhibited = false; return 0; }
bool vision_badge_power_network_idle(void) { return !network_busy || ticks >= 500; }
int camera_service_control(const char *action, char *text, size_t size)
{
  (void)text; (void)size;
  assert(inhibited);
  if (!strcmp(action, "pause"))
    { pause_calls++; if (camera_error) return camera_error; assert(!camera_owned); camera_owned = true; }
  else { assert(!strcmp(action, "resume") && camera_owned); camera_owned = false; return camera_resume_error; }
  return 0;
}
int bk7258_aidk_eye_power(bool enabled)
{
  assert(inhibited && (camera_owned || enabled));
  display_calls++; display_enabled = enabled; /* Timeout may have applied. */
  return display_error;
}
void sync(void) { assert(inhibited && camera_owned && !display_enabled); sync_calls++; }
int bk7258_pm_soft_off_request(void) { assert(sync_calls && camera_owned && !display_enabled); off_calls++; return off_error; }
int bk7258_pm_soft_off_status(void) { return pending; }
static char text[256];
static int command(const char *action) { return power_service_command(action, text, sizeof(text)); }

int main(int argc, char **argv)
{
  assert(argc == 2 && power_service_work_allowed());
  assert(command("invalid") == -EINVAL && !off_calls && !inhibited);
  assert(power_service_command(NULL, text, sizeof(text)) == -EINVAL);
  assert(power_service_command("status", NULL, sizeof(text)) == -EINVAL);
  assert(command("status") == 0 && strstr(text, "active"));
  if (!strcmp(argv[1], "quiet"))
    {
      voice_busy = 3;
      assert(command("quiet") == 0 && waits == 3 && !power_service_work_allowed());
      assert(camera_owned && !display_enabled && pause_calls == 1 && !off_calls && !sync_calls);
      assert(command("quiet") == 0 && pause_calls == 1);
      assert(command("prepare") == 0 && sync_calls == 1 && !off_calls);
      assert(command("toggle") == 0 && power_service_work_allowed() && !inhibited && !camera_owned && display_enabled);
    }
  else if (!strcmp(argv[1], "off") || !strcmp(argv[1], "lost-ack") || !strcmp(argv[1], "unknown"))
    {
      off_error = strcmp(argv[1], "off") ? -ETIMEDOUT : 0;
      pending = !strcmp(argv[1], "unknown") ? -ENOTCONN : 1;
      assert(command("toggle") == off_error && off_calls == 1 && !power_service_work_allowed());
      assert(command("off") == -EINPROGRESS && off_calls == 1);
      assert(command("resume") == -EINPROGRESS && inhibited);
      assert(command("toggle") == -EINPROGRESS && inhibited);
      assert(command("status") == 0 && strstr(text, pending < 0 ? "unknown" : "pending"));
      pending = 0;
      assert(command("status") == 0 && strstr(text, "failed"));
      assert(command("resume") == 0 && power_service_work_allowed());
    }
  else if (!strcmp(argv[1], "voice-timeout") || !strcmp(argv[1], "retained"))
    {
      if (!strcmp(argv[1], "retained")) voice_error = -EIO; else voice_busy = 100000;
      assert(command("off") == (voice_error ? -EIO : -ETIMEDOUT));
      assert(!pause_calls && !display_calls && !off_calls && !power_service_work_allowed());
      assert(command("off") < 0 && !off_calls);
    }
  else if (!strcmp(argv[1], "camera-fail"))
    {
      camera_error = -EIO;
      assert(command("off") == -EIO && !display_calls && !off_calls);
      camera_error = 0;
      assert(command("resume") == -EIO && !power_service_work_allowed() && inhibited);
      assert(strstr(text, "reboot_required=1") && !off_calls);
    }
  else if (!strcmp(argv[1], "camera-resume-fail"))
    {
      assert(command("quiet") == 0);
      camera_resume_error = -EIO;
      assert(command("resume") == -EIO && !camera_owned && !power_service_work_allowed());
      camera_resume_error = 0;
      assert(command("resume") == -EIO && inhibited && strstr(text, "reboot_required=1"));
    }
  else if (!strcmp(argv[1], "display-timeout"))
    {
      display_error = -ETIMEDOUT;
      assert(command("off") == -ETIMEDOUT && camera_owned && !display_enabled && !off_calls);
      assert(command("resume") == -ETIMEDOUT && !power_service_work_allowed());
      display_error = 0;
      assert(command("resume") == 0 && display_enabled && !camera_owned);
    }
  else if (!strcmp(argv[1], "resume-fail"))
    {
      assert(command("quiet") == 0);
      resume_error = -EIO;
      assert(command("resume") == -EIO && !power_service_work_allowed());
      resume_error = 0;
      assert(command("resume") == 0 && power_service_work_allowed());
    }
  else
    {
      assert(!strcmp(argv[1], "network-busy")); network_busy = true;
      assert(command("prepare") == 0 && ticks >= 500 && !off_calls);
    }
  printf("POWER_COORDINATOR_PASS %s (owners/CP mocked; no sleep/current proof)\n", argv[1]);
  return 0;
}
