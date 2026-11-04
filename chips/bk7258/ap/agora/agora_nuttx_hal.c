/* SPDX-License-Identifier: Apache-2.0 */
/* Chip-owned NuttX implementation of the pinned Agora AOSL interface.
 * The vendor AOSL sources and RTC archives retain their own licences.
 */
#ifndef VISION_AGORA_HOST_TEST
#  include <nuttx/config.h>
#  include <nuttx/spinlock.h>
#  include <arch/chip/bk7258_psram.h>
#  if !defined(CONFIG_BK7258_PSRAM_SYSTEM_HEAP) || !defined(CONFIG_BK7258_PSRAM_EXT_SYSTEM_HEAP)
#    error "Agora requires the AP PSRAM-backed NuttX system heap"
#  endif
#  if CONFIG_BK7258_PSRAM_EXT_SYSTEM_HEAP_SIZE < 0x400000
#    error "Agora integration requires the verified 4MiB AP high-bank heap"
#  endif
#endif
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/utsname.h>
#include <hal/aosl_hal_atomic.h>
#include <hal/aosl_hal_memory.h>
#include <hal/aosl_hal_thread.h>
#include <hal/aosl_hal_errno.h>
#include <hal/aosl_hal_iomp.h>
#include <hal/aosl_hal_time.h>
#include <hal/aosl_hal_file.h>
#include <hal/aosl_hal_log.h>
#include <hal/aosl_hal_utils.h>

void *aosl_hal_malloc(size_t size)
{
#ifndef VISION_AGORA_HOST_TEST
  if (!bk7258_psram_ready() ||
      !bk7258_psram_system_heap_contains((void *)(uintptr_t)(BK7258_PSRAM_AP_EXT_HEAP_BASE + 1)))
    { errno = ENODEV; return NULL; }
#endif
  /* AP startup already adds the 4MiB high PSRAM bank to NuttX's heap.
   * The private 640KiB allocator donated 512KiB to that same system heap;
   * allocating RTC buffers from its remaining <128KiB cannot work.
   * Standard malloc uses SRAM for small objects and the PSRAM regions for
   * large objects, preserving the original allocator's alignment and locks.
   */
  return malloc(size);
}
void aosl_hal_free(void *p)
{
  free(p);
}
void *aosl_hal_calloc(size_t n, size_t size)
{
  void *p;
  if (size && n > SIZE_MAX / size) { errno = ENOMEM; return NULL; }
  p = aosl_hal_malloc(n * size);
  if (p) memset(p, 0, n * size);
  return p;
}
void *aosl_hal_realloc(void *p, size_t size)
{
  if (!size) { aosl_hal_free(p); return NULL; }
#ifndef VISION_AGORA_HOST_TEST
  if (!bk7258_psram_ready() ||
      !bk7258_psram_system_heap_contains((void *)(uintptr_t)(BK7258_PSRAM_AP_EXT_HEAP_BASE + 1)))
    { errno = ENODEV; return NULL; }
#endif
  return realloc(p, size);
}

/* BK7258 PSRAM cannot complete exclusive stores. Keep the gate in SRAM,
 * and use ordinary loads/stores under it, even for SDK counters in PSRAM.
 */
#ifdef VISION_AGORA_HOST_TEST
static pthread_mutex_t g_atomic_lock = PTHREAD_MUTEX_INITIALIZER;
typedef int atomic_guard_t;
static atomic_guard_t atomic_lock(void)
{ pthread_mutex_lock(&g_atomic_lock); return 0; }
static void atomic_unlock(atomic_guard_t flags)
{ (void)flags; pthread_mutex_unlock(&g_atomic_lock); }
#else
static spinlock_t g_atomic_lock = SP_UNLOCKED;
typedef irqstate_t atomic_guard_t;
static atomic_guard_t atomic_lock(void)
{ return spin_lock_irqsave(&g_atomic_lock); }
static void atomic_unlock(atomic_guard_t flags)
{ spin_unlock_irqrestore(&g_atomic_lock, flags); }
#endif
intptr_t aosl_hal_atomic_read(const intptr_t *v)
{
  atomic_guard_t f = atomic_lock();
  intptr_t r = *v;
  atomic_unlock(f); return r;
}
void aosl_hal_atomic_set(intptr_t *v, intptr_t i)
{
  atomic_guard_t f = atomic_lock(); *v = i; atomic_unlock(f);
}
static intptr_t counter_add(intptr_t *v, uintptr_t delta, bool old_value)
{
  atomic_guard_t f = atomic_lock();
  intptr_t old = *v;
  *v = (intptr_t)((uintptr_t)old + delta);
  intptr_t r = old_value ? old : *v;
  atomic_unlock(f); return r;
}
intptr_t aosl_hal_atomic_inc(intptr_t *v) { return counter_add(v, 1, true); }
intptr_t aosl_hal_atomic_dec(intptr_t *v) { return counter_add(v, UINTPTR_MAX, true); }
intptr_t aosl_hal_atomic_add(intptr_t i, intptr_t *v)
{ return counter_add(v, (uintptr_t)i, false); }
intptr_t aosl_hal_atomic_sub(intptr_t i, intptr_t *v)
{ return counter_add(v, (uintptr_t)0 - (uintptr_t)i, false); }
intptr_t aosl_hal_atomic_xchg(intptr_t *v, intptr_t i)
{
  atomic_guard_t f = atomic_lock(); intptr_t old = *v;
  *v = i; atomic_unlock(f); return old;
}
intptr_t aosl_hal_atomic_cmpxchg(intptr_t *v, intptr_t old, intptr_t next)
{
  atomic_guard_t f = atomic_lock(); intptr_t r = *v;
  if (r == old) *v = next;
  atomic_unlock(f); return r;
}
void aosl_hal_mb(void) { __sync_synchronize(); }
void aosl_hal_rmb(void) { __sync_synchronize(); }
void aosl_hal_wmb(void) { __sync_synchronize(); }

static int thread_priority(aosl_thread_proiority_e pri)
{
  static const int priorities[] = {100, 95, 100, 105, 110, 115};
  return (unsigned int)pri < sizeof(priorities) / sizeof(priorities[0]) ?
         priorities[pri] : -1;
}
int aosl_hal_thread_create(aosl_thread_t *thread, aosl_thread_param_t *param,
                          void *(*entry)(void *), void *arg)
{
  pthread_attr_t attr;
  pthread_t id;
  int ret;
  if (!thread || !entry || (param && (param->stack_size < 0 ||
      thread_priority(param->priority) < 0))) return -EINVAL;
  ret = pthread_attr_init(&attr);
  if (ret) return -ret;
  size_t stack = param && param->stack_size ? (size_t)param->stack_size : 32768;
  /* BK's reference HAL always allocates 12KiB, even when the AOSL LTWP
   * requests 4KiB. Preserve that platform floor for SDK worker call chains. */
  if (stack < 12u * 1024u) stack = 12u * 1024u;
  if (stack < (size_t)PTHREAD_STACK_MIN) stack = PTHREAD_STACK_MIN;
  ret = pthread_attr_setstacksize(&attr, stack);
#ifndef VISION_AGORA_HOST_TEST
  if (!ret)
    {
      cpu_set_t cpus;
      CPU_ZERO(&cpus); CPU_SET(0, &cpus);
      ret = pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
    }
#endif
#if !defined(VISION_AGORA_HOST_TEST) || defined(VISION_AGORA_SCHED_TEST)
  if (!ret)
    {
      /* DEFAULT and a NULL parameter also mean the SDK's normal100, not
       * the caller's voice priority. All SDK workers stay in95..115. */
      struct sched_param p =
        { .sched_priority = thread_priority(param ? param->priority : AOSL_THRD_PRI_DEFAULT) };
      ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
      if (!ret) ret = pthread_attr_setschedpolicy(&attr, SCHED_RR);
      if (!ret) ret = pthread_attr_setschedparam(&attr, &p);
    }
#endif
  if (!ret) ret = pthread_create(&id, &attr, entry, arg);
  pthread_attr_destroy(&attr);
  if (ret) return -ret;
  *thread = (aosl_thread_t)id;
  return 0;
}
void aosl_hal_thread_destroy(aosl_thread_t t) { (void)pthread_cancel((pthread_t)t); }
void aosl_hal_thread_exit(void *result) { pthread_exit(result); }
aosl_thread_t aosl_hal_thread_self(void) { return (aosl_thread_t)pthread_self(); }
void aosl_hal_thread_detach(aosl_thread_t t) { (void)pthread_detach((pthread_t)t); }
int aosl_hal_thread_join(aosl_thread_t t, void **result)
{ return -pthread_join((pthread_t)t, result); }
int aosl_hal_thread_set_name(const char *name)
{
  char shortname[16];
  if (!name) return -EINVAL;
  snprintf(shortname, sizeof(shortname), "%s", name);
  return -pthread_setname_np(pthread_self(), shortname);
}
int aosl_hal_thread_set_priority(aosl_thread_proiority_e pri)
{
  int priority = thread_priority(pri);
  if (priority < 0) return -EINVAL;
#ifdef VISION_AGORA_HOST_TEST
  return 0; /* Host regression does not require RT scheduling privileges. */
#else
  struct sched_param p = { .sched_priority = priority };
  return -pthread_setschedparam(pthread_self(), SCHED_RR, &p);
#endif
}
aosl_mutex_t aosl_hal_mutex_create(void)
{
  pthread_mutex_t *m = calloc(1, sizeof(*m));
  if (m && pthread_mutex_init(m, NULL)) { free(m); m = NULL; }
  return m;
}
void aosl_hal_mutex_destroy(aosl_mutex_t m)
{ if (m && pthread_mutex_destroy(m) == 0) free(m); }
int aosl_hal_mutex_lock(aosl_mutex_t m) { return m ? -pthread_mutex_lock(m) : -EINVAL; }
int aosl_hal_mutex_trylock(aosl_mutex_t m) { return m ? -pthread_mutex_trylock(m) : -EINVAL; }
int aosl_hal_mutex_unlock(aosl_mutex_t m) { return m ? -pthread_mutex_unlock(m) : -EINVAL; }
aosl_cond_t aosl_hal_cond_create(void)
{
  pthread_cond_t *c = calloc(1, sizeof(*c));
  pthread_condattr_t attr;
  int ret;
  if (!c) return NULL;
  ret = pthread_condattr_init(&attr);
  if (ret) { free(c); return NULL; }
  ret = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  if (!ret) ret = pthread_cond_init(c, &attr);
  pthread_condattr_destroy(&attr);
  if (ret) { free(c); return NULL; }
  return c;
}
void aosl_hal_cond_destroy(aosl_cond_t c)
{ if (c && pthread_cond_destroy(c) == 0) free(c); }
int aosl_hal_cond_signal(aosl_cond_t c) { return c ? -pthread_cond_signal(c) : -EINVAL; }
int aosl_hal_cond_broadcast(aosl_cond_t c) { return c ? -pthread_cond_broadcast(c) : -EINVAL; }
int aosl_hal_cond_wait(aosl_cond_t c, aosl_mutex_t m)
{ return c && m ? -pthread_cond_wait(c, m) : -EINVAL; }
int aosl_hal_cond_timedwait(aosl_cond_t c, aosl_mutex_t m, intptr_t ms)
{
  struct timespec deadline;
  if (!c || !m) return -EINVAL;
  if (ms < 0) return aosl_hal_cond_wait(c, m);
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0) return -errno;
  deadline.tv_sec += ms / 1000;
  deadline.tv_nsec += (ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
  return -pthread_cond_timedwait(c, m, &deadline);
}

int aosl_hal_errno_convert(int err)
{
  if (!err) return AOSL_HAL_RET_SUCCESS;
  if (err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS)
    return AOSL_HAL_RET_EAGAIN;
  if (err == EINTR) return AOSL_HAL_RET_EINTR;
  return AOSL_HAL_RET_FAILURE;
}
fd_set_t aosl_hal_fdset_create(void) { return calloc(1, sizeof(fd_set)); }
void aosl_hal_fdset_destroy(fd_set_t f) { free(f); }
void aosl_hal_fdset_zero(fd_set_t f) { if (f) FD_ZERO((fd_set *)f); }
void aosl_hal_fdset_set(fd_set_t f, int fd)
{ if (f && fd >= 0 && fd < FD_SETSIZE) FD_SET(fd, (fd_set *)f); }
void aosl_hal_fdset_clr(fd_set_t f, int fd)
{ if (f && fd >= 0 && fd < FD_SETSIZE) FD_CLR(fd, (fd_set *)f); }
int aosl_hal_fdset_isset(fd_set_t f, int fd)
{ return f && fd >= 0 && fd < FD_SETSIZE && FD_ISSET(fd, (fd_set *)f); }
int aosl_hal_select(int n, fd_set_t r, fd_set_t w, fd_set_t e, int ms)
{
  struct timeval tv = {0};
  if (n < 0 || n > FD_SETSIZE) { errno = EINVAL; return -1; }
  if (ms >= 0) { tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000; }
  int ret = select(n, r, w, e, ms < 0 ? NULL : &tv);
  return ret < 0 ? aosl_hal_errno_convert(errno) : ret;
}
static uint64_t clock_ms(clockid_t clock)
{
  struct timespec t;
  if (clock_gettime(clock, &t) < 0) return 0;
  return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
uint64_t aosl_hal_get_tick_ms(void) { return clock_ms(CLOCK_MONOTONIC); }
uint64_t aosl_hal_get_time_ms(void) { return clock_ms(CLOCK_REALTIME); }
int aosl_hal_get_time_str(char *buf, int len)
{
  time_t now = time(NULL);
  struct tm tm;
  if (!buf || len < 21 || !gmtime_r(&now, &tm)) return -EINVAL;
  return strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm) ? 0 : -ENOSPC;
}
void aosl_hal_msleep(uint64_t ms)
{
  /* Bound conversion on 32-bit time_t; interrupted sleeps retain remaining time. */
  while (ms)
    {
      uint64_t part = ms > 60000 ? 60000 : ms;
      struct timespec t = { .tv_sec = part / 1000, .tv_nsec = (part % 1000) * 1000000 };
      while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
      ms -= part;
    }
}
int aosl_hal_mkdir(const char *p) { return mkdir(p, 0700); }
int aosl_hal_rmdir(const char *p) { return rmdir(p); }
int aosl_hal_fexist(const char *p) { return access(p, F_OK) == 0; }
int aosl_hal_fsize(const char *p)
{
  struct stat st;
  if (stat(p, &st) < 0) return -1;
  if (st.st_size < 0 || st.st_size > INT_MAX) { errno = EOVERFLOW; return -1; }
  return (int)st.st_size;
}
int aosl_hal_file_create(const char *p)
{ int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600); return fd < 0 ? -1 : close(fd); }
int aosl_hal_file_delete(const char *p) { return unlink(p); }
int aosl_hal_file_rename(const char *a, const char *b) { return rename(a, b); }
aosl_fs_t aosl_hal_fopen(const char *p, const char *m) { return fopen(p, m); }
int aosl_hal_fclose(aosl_fs_t f) { return f ? fclose(f) : -1; }
size_t aosl_hal_fread(aosl_fs_t f, void *p, size_t size)
{ if (!f) return (size_t)-1; size_t n = fread(p, 1, size, f); return ferror(f) ? (size_t)-1 : n; }
size_t aosl_hal_fwrite(aosl_fs_t f, const void *p, size_t size)
{ if (!f) return (size_t)-1; size_t n = fwrite(p, 1, size, f); return ferror(f) ? (size_t)-1 : n; }
int aosl_hal_printf(const char *format, va_list args) { return vprintf(format, args); }
int aosl_hal_get_uuid(char *buf, int size)
{
  unsigned char random[16];
  size_t used = 0;
  int fd;
  if (!buf || size < 33) return -EINVAL;
#ifdef VISION_AGORA_HOST_TEST
  fd = open("/dev/urandom", O_RDONLY);
#else
  /* The AP profile enables BK7258_TRNG/DEV_RANDOM, not DEV_URANDOM.
   * Its hardware driver returns all requested bytes or an error.
   */
  fd = open("/dev/random", O_RDONLY);
#endif
  if (fd < 0) return -errno;
  while (used < sizeof(random))
    {
      ssize_t n = read(fd, random + used, sizeof(random) - used);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) { int ret = n < 0 ? -errno : -EIO; close(fd); return ret; }
      used += n;
    }
  close(fd);
  for (used = 0; used < sizeof(random); used++) snprintf(buf + used * 2, 3, "%02x", random[used]);
  return 0;
}
int aosl_hal_os_version(char *buf, int size)
{
  struct utsname info;
  if (!buf || size <= 0) return -EINVAL;
  if (uname(&info) < 0) return -errno;
  int n = snprintf(buf, size, "%s %s", info.sysname, info.release);
  return n < 0 || n >= size ? -ENOSPC : 0;
}
