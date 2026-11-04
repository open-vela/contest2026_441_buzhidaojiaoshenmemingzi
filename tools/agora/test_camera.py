#!/usr/bin/env python3
"""Compile the actual camera service on Linux with V4L2/clock-only mocks.

No sensor, photo colors or hardware performance is verified by this test.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "app/vision_badge/src/camera_service.c").read_text()
source = re.sub(r"^\s*#\s*include[^\n]*", "", source, flags=re.M)
header = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <syslog.h>
#include <sys/types.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#define __NuttX__ 1
#define CONFIG_CONTEST2026_441_VISION_BADGE_CAMERA_DEVPATH "/dev/video0"
#define CONFIG_CONTEST2026_441_VISION_BADGE_JPEG_BUFFER_SIZE 262144
#define VISION_BADGE_IMAGE_JPEG 1
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_FAILED ((void *)-1)
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define V4L2_MEMORY_MMAP 1
#define V4L2_PIX_FMT_JPEG 1
#define V4L2_FIELD_ANY 0
#define V4L2_BUF_FLAG_ERROR 1
enum { VIDIOC_S_FMT=1, VIDIOC_REQBUFS, VIDIOC_QUERYBUF, VIDIOC_QBUF,
       VIDIOC_STREAMON, VIDIOC_DQBUF, VIDIOC_STREAMOFF };
struct v4l2_requestbuffers { unsigned type, memory, count; };
struct v4l2_format { unsigned type; struct { struct { unsigned width, height,
 pixelformat, field; size_t sizeimage; } pix; } fmt; };
struct v4l2_buffer { unsigned type, memory, index, flags; size_t length, bytesused;
 struct { unsigned offset; } m; };
struct vision_badge_image_s { void *data; size_t capacity, size; unsigned width, height, format; };
struct vision_badge_probe_s { bool available; int error_code; };
static uint64_t tick;
static _Atomic int fail_at, qfail, polls, opens, closes, stops, released, maps, unmaps, dq;
static _Atomic bool interrupt_poll, never_poll, stall_pair, driver_buffers_live;
static _Atomic bool readable, late_poll, late_done, spurious_ready;
static bool warm_test;
static unsigned char pixels[2][8];
static int fake_clock(int id, struct timespec *ts)
{ if(warm_test) return clock_gettime(id,ts);
  (void)id; ts->tv_sec=tick/1000; ts->tv_nsec=(tick%1000)*1000000; return 0; }
static int fake_open(const char *path, int mode)
{ (void)path; (void)mode; opens++; if(fail_at==100) { errno=EIO; return -1; } return 9; }
static int fake_close(int fd)
{ assert(fd==9); closes++; if(fail_at==101) { errno=EIO; return -1; }
  driver_buffers_live=false; return 0; }
static int fake_ioctl(int fd, int op, uintptr_t arg)
{
 assert(fd==9);
 if(op==VIDIOC_STREAMOFF) stops++;
 if(op==VIDIOC_REQBUFS && ((struct v4l2_requestbuffers *)arg)->count==0) released++;
 if(op==fail_at || (op==VIDIOC_QBUF && qfail && dq>0)) { errno=EIO; return -1; }
 if(op==VIDIOC_REQBUFS && ((struct v4l2_requestbuffers *)arg)->count>0)
   driver_buffers_live=true;
 if(op==VIDIOC_QUERYBUF) { struct v4l2_buffer *b=(void *)arg; b->length=8; b->m.offset=b->index; }
 if(op==VIDIOC_DQBUF) {
   if(warm_test && (!atomic_load(&readable) || atomic_exchange(&spurious_ready,false)))
     { errno=EAGAIN;return -1; }
   if(warm_test) atomic_store(&readable,false);
   struct v4l2_buffer *b=(void *)arg; dq++; b->index=dq%2; b->bytesused=8;
   unsigned char value[8]={255,216,255,218,(unsigned char)dq,0,255,217};
   memcpy(pixels[b->index], value, 8);
   if(fail_at==102) b->index=2;
   if(fail_at==103) pixels[b->index][0]=0;
   if(fail_at==104) b->bytesused=9;
 }
 return 0;
}
static void *fake_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off)
{ (void)p;(void)n;(void)prot;(void)flags;assert(fd==9 && driver_buffers_live);maps++;
  if(fail_at==105 && maps==2) { errno=ENOMEM; return MAP_FAILED; } return pixels[off]; }
#if !defined(CONFIG_BK7258_AIDK_CAMERA) || !defined(CONFIG_BUILD_FLAT)
static int fake_munmap(void *p, size_t n)
{ (void)p;(void)n;unmaps++;if(fail_at==106){errno=EIO;return -1;}return 0; }
#endif
static int fake_poll(struct pollfd *fds, unsigned n, int timeout)
{
 assert(n==1 && timeout>0); polls++;
 if(warm_test) {
   if(atomic_exchange(&late_poll,false)) {
     /* Poll timed out, but its caller was descheduled. A frame is now
      * queued when it resumes; the old timeout result is no longer fresh. */
     usleep(2100000);atomic_store(&readable,true);atomic_store(&late_done,true);
     fds->revents=0;return 0;
   }
   usleep(30000);
   if(never_poll) { usleep((unsigned)timeout*1000);return 0; }
   atomic_store(&readable,true);fds->revents=POLLIN;return 1;
 }
 if(interrupt_poll) { tick+=1000;errno=EINTR;return -1; }
 if(never_poll || (stall_pair && dq>0)) { tick+=timeout;return 0; }
 tick+=30;fds->revents=POLLIN;return 1;
}
#define clock_gettime fake_clock
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define mmap fake_mmap
#define munmap fake_munmap
#define poll fake_poll
#ifdef CONFIG_BK7258_AP_CORE
typedef pthread_mutex_t mutex_t;
#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
static int nxmutex_lock(mutex_t *lock) { return -pthread_mutex_lock(lock); }
static int nxmutex_unlock(mutex_t *lock) { return -pthread_mutex_unlock(lock); }
static int nxsem_init(sem_t *sem, int shared, unsigned value) { return sem_init(sem,shared,value); }
static int nxsem_destroy(sem_t *sem) { return sem_destroy(sem); }
static int nxsem_post(sem_t *sem) { return sem_post(sem); }
static int nxsem_wait_uninterruptible(sem_t *sem)
{ int ret;do { ret=sem_wait(sem); }while(ret<0 && errno==EINTR);return ret; }
static int nxsig_usleep(unsigned us) { return usleep(us); }
static int (*worker_entry)(int,char **);
static void *worker_trampoline(void *arg) { (void)arg;worker_entry(0,NULL);return NULL; }
static int kthread_create(const char *name, int priority, int stack,
                         int (*entry)(int,char **), char *const *argv)
{
 (void)name;(void)priority;(void)stack;(void)argv;
 if(fail_at==108) return -ENOMEM;
 worker_entry=entry;pthread_t thread;assert(!pthread_create(&thread,NULL,worker_trampoline,NULL));
 assert(!pthread_detach(thread));return 1;
}
static void *fake_malloc(size_t size) { return fail_at==107 ? NULL : malloc(size); }
#define malloc fake_malloc
#endif
'''
main = r'''
static void reset(void)
{ tick=100;fail_at=qfail=polls=opens=closes=stops=released=maps=unmaps=dq=0;
  interrupt_poll=never_poll=stall_pair=driver_buffers_live=false; }
static void check_unmaps(void)
{
  /* REQBUFS(count=0) is a no-op in this NuttX capture implementation;
   * the final successful close, not that ioctl or munmap, releases buffers.
   */
  if(fail_at!=101) assert(!driver_buffers_live);
#if defined(CONFIG_BK7258_AIDK_CAMERA) && defined(CONFIG_BUILD_FLAT)
  /* Actual capture_mmap contract: no mm_map registration; last close frees
   * the driver's buffer. Calling munmap would be an EINVAL regression.
   */
  assert(unmaps==0);
#else
  assert(unmaps==maps-(fail_at==105?1:0));
#endif
}
int main(void)
{
 unsigned char a[32], b[32];
 struct vision_badge_image_s early={.data=a,.capacity=sizeof(a)}, late={.data=b,.capacity=sizeof(b)};
 assert(camera_service_capture(NULL)==-EINVAL);
 assert(camera_service_capture_pair(&early,NULL)==-EINVAL);
 assert(camera_service_capture_pair(&early,&early)==-EINVAL);
 reset(); assert(camera_service_capture(&early)==0 && early.size==8 && dq>1);
 assert(tick>=1130 && tick<2130 && a[4]==dq);
 assert(opens==1 && closes==1 && stops==1 && released==1);check_unmaps();
 reset(); assert(camera_service_capture_pair(&early,&late)==0);
 assert(early.size==8 && late.size==8 && a[4]==1 && b[4]>1 && tick>=1130);
 assert(opens==1 && closes==1 && stops==1 && released==1);check_unmaps();
 reset(); qfail=1; assert(camera_service_capture_pair(&early,&late)==-EIO);
 assert(!early.size && !late.size && closes==1 && stops==1);check_unmaps();
 reset(); qfail=1; assert(camera_service_capture(&early)==-EIO && !early.size);
 assert(dq==1 && closes==1 && stops==1);check_unmaps();
 int failures[]={100,VIDIOC_S_FMT,VIDIOC_REQBUFS,VIDIOC_QUERYBUF,VIDIOC_QBUF,
  VIDIOC_STREAMON,VIDIOC_DQBUF,VIDIOC_STREAMOFF,101,102,103,104,105
#if !defined(CONFIG_BK7258_AIDK_CAMERA) || !defined(CONFIG_BUILD_FLAT)
  ,106
#endif
 };
 for(unsigned i=0;i<sizeof(failures)/sizeof(failures[0]);i++) {
   reset();fail_at=failures[i];assert(camera_service_capture(&early)<0 && !early.size);
   assert(closes==(fail_at==100?0:1));check_unmaps();
 }
 reset(); interrupt_poll=true;
 assert(camera_service_capture(&early)==-ETIMEDOUT && polls==5 && closes==1 && stops==1);
 reset(); never_poll=true;assert(camera_service_capture_pair(&early,&late)==-ETIMEDOUT && !late.size);
 reset();stall_pair=true;
 assert(camera_service_capture_pair(&early,&late)==-ETIMEDOUT && !early.size && !late.size);
 assert(closes==1 && stops==1);check_unmaps();
 reset();stall_pair=true;
 assert(camera_service_capture(&early)==-ETIMEDOUT && !early.size && dq==1);
 assert(tick==2130 && closes==1 && stops==1);check_unmaps();
 for(int run=0;run<3;run++) {
   reset();assert(camera_service_capture(&early)==0 && a[4]==dq && dq>1);
   assert(opens==1 && closes==1);check_unmaps();
 }
 reset();early.capacity=1;assert(camera_service_capture(&early)==-EFBIG && !early.size);
 puts("PASS: actual camera source, settled single/early-late pair, repeated fresh-frame requeue, bounded settling/EINTR timeout and cleanup failures; hardware mocked");
 return 0;
}
'''
warm_main = r'''
static bool cancel_snapshot(void *arg) { return *(bool *)arg; }
static void wait_ready(void)
{
 for(unsigned i=0;i<400;i++) {
   nxmutex_lock(&g_camera.lock);bool ready=g_camera.ready;nxmutex_unlock(&g_camera.lock);
   if(ready) return;
   usleep(10000);
 }
 assert(!"warm camera never became ready");
}
static void wait_disabled(void)
{
 for(unsigned i=0;i<400;i++) {
   nxmutex_lock(&g_camera.lock);bool done=!g_camera.busy && !g_camera.wanted;nxmutex_unlock(&g_camera.lock);
   if(done) return;
   usleep(10000);
 }
 assert(!"warm camera failed to clean up");
}
int main(void)
{
 warm_test=true;
 unsigned char data[32];char text[256];
 struct vision_badge_image_s image={.data=data,.capacity=sizeof(data)};
 assert(camera_service_control(NULL,text,sizeof(text))==-EINVAL);
 assert(camera_service_capture(NULL)==-EINVAL);
 assert(camera_service_capture_pair(&image,&image)==-EINVAL);
 assert(opens==0);
 fail_at=108;assert(camera_service_control("start",text,sizeof(text))==-ENOMEM);
 fail_at=0;assert(camera_service_control("start",text,sizeof(text))==0);
 wait_ready();assert(opens==1 && !closes && driver_buffers_live);
 for(unsigned i=0;i<3;i++) {
   uint64_t started=camera_now_ms();
   nxmutex_lock(&g_camera.lock);uint32_t previous=g_camera.sequence;nxmutex_unlock(&g_camera.lock);
   assert(camera_service_capture(&image)==0 && image.size==8);
   assert(camera_now_ms()-started<600); /* mock-only, not a hardware promise */
   nxmutex_lock(&g_camera.lock);assert((uint32_t)(g_camera.sequence-previous)>=3);nxmutex_unlock(&g_camera.lock);
   assert(opens==1 && !closes);
 }
 /* Borrow the existing worker: no reopen, old frame, or competing owner. */
 int before_stall_opens=opens,before_stall_closes=closes;
 atomic_store(&late_done,false);atomic_store(&late_poll,true);
 for(unsigned i=0;i<400 && !atomic_load(&late_done);i++) usleep(10000);
 assert(atomic_load(&late_done));
 assert(camera_service_capture(&image)==0 && image.size==8);
 atomic_store(&spurious_ready,true);
 assert(camera_service_capture(&image)==0 && image.size==8);
 assert(opens==before_stall_opens && closes==before_stall_closes);
 puts("CAMERA_POLL_RECOVERY_PASS delayed timeout with queued frame and spurious readiness; no reopen; device/scheduling mocked");
 int live_opens=opens,live_closes=closes;
 assert(camera_service_control("borrow",text,sizeof(text))==0);
 assert(g_camera.reserved && !g_camera.paused && driver_buffers_live);
 assert(camera_service_control("borrow",text,sizeof(text))==-EBUSY);
 assert(camera_service_control("start",text,sizeof(text))==-EBUSY);
 assert(camera_service_control("stop",text,sizeof(text))==-EBUSY);
 assert(camera_service_control("pause",text,sizeof(text))==-EBUSY);
 assert(camera_service_capture(&image)==-EBUSY && !image.size);
 for(unsigned i=0;i<3;i++) {
   nxmutex_lock(&g_camera.lock);uint32_t previous=g_camera.sequence;nxmutex_unlock(&g_camera.lock);
   assert(camera_service_capture_live(&image,500,NULL,NULL)==0 && image.size==8);
   nxmutex_lock(&g_camera.lock);assert((uint32_t)(g_camera.sequence-previous)>=3);nxmutex_unlock(&g_camera.lock);
 }
 bool canceled=true;
 assert(camera_service_capture_live(&image,500,cancel_snapshot,&canceled)==-ECANCELED && !image.size);
 assert(camera_service_capture_live(&image,1,NULL,NULL)==-ETIMEDOUT && !image.size);
 image.capacity=1;assert(camera_service_capture_live(&image,500,NULL,NULL)==-EFBIG && !image.size);
 image.capacity=sizeof(data);
 /* A live owner remains reserved after the real worker's idle timeout.
  * Cancel must not restart it; the next valid request reopens it once. */
 never_poll=true;wait_disabled();
 assert(g_camera.reserved && !g_camera.blocked && g_camera.error==-ETIMEDOUT);
 int recovery_opens=opens,recovery_closes=closes;
 assert(camera_service_capture_live(&image,2000,cancel_snapshot,&canceled)==-ECANCELED && !image.size);
 assert(opens==recovery_opens && closes==recovery_closes);
 never_poll=false;
 uint32_t recovery_sequence=g_camera.sequence;
 assert(camera_service_capture_live(&image,2000,NULL,NULL)==0 && image.size==8);
 assert(opens==recovery_opens+1 && closes==recovery_closes);
 assert(g_camera.reserved && !g_camera.paused && g_camera.sequence-recovery_sequence>=3);
 /* A persistent stall must fail within this request, without repeated
  * reopen loops or a new timeout budget. A later request may try again. */
 never_poll=true;wait_disabled();recovery_opens=opens;
 uint64_t recovery_started=camera_now_ms();
 assert(camera_service_capture_live(&image,2500,NULL,NULL)==-ETIMEDOUT && !image.size);
 assert(camera_now_ms()-recovery_started<2800 && opens==recovery_opens+1);
 wait_disabled();never_poll=false;
 assert(camera_service_capture_live(&image,2000,NULL,NULL)==0 && image.size==8);
 live_opens=opens;live_closes=closes;
 puts("CAMERA_LIVE_TIMEOUT_RECOVERY_PASS actual worker close/reopen, retained owner, fresh frames, cancellation and single bounded retry; device mocked");
 assert(camera_service_control("resume",text,sizeof(text))==0 && !g_camera.reserved);
 assert(opens==live_opens && closes==live_closes && driver_buffers_live);
 assert(camera_service_capture_live(&image,500,NULL,NULL)==-EBUSY && !image.size);
 puts("CAMERA_LIVE_OWNER_PASS fresh frames, no reopen, cancellation/deadline, exclusive borrowing; device mocked");
 image.capacity=1;assert(camera_service_capture(&image)==-EFBIG && !image.size);
 image.capacity=sizeof(data);
 assert(camera_service_control("stop",text,sizeof(text))==0);
 assert(!driver_buffers_live && closes==live_closes+1 && stops==live_closes+1);
 nxmutex_lock(&g_camera.lock);assert(!g_camera.jpeg && !g_camera.ready);nxmutex_unlock(&g_camera.lock);
 assert(camera_service_control("start",text,sizeof(text))==0);wait_ready();
 assert(opens==live_opens+1);assert(camera_service_capture(&image)==0);
 /* Cold diagnostic remains available and resumes a previously warm stream. */
 unsigned char other[32];struct vision_badge_image_s late={.data=other,.capacity=sizeof(other)};
 assert(camera_service_capture_pair(&image,&late)==0 && image.size==8 && late.size==8);
 wait_ready();assert(camera_service_control("status",text,sizeof(text))==0);
 assert(strstr(text,"ready=1"));
 /* A voice takeover closes capture, rejects snapshots and restores intent. */
 assert(camera_service_control("pause",text,sizeof(text))==0);
 assert(!driver_buffers_live && strstr(text,"paused=1"));
 assert(camera_service_control("pause",text,sizeof(text))==-EBUSY);
 assert(camera_service_control("start",text,sizeof(text))==-EBUSY);
 assert(camera_service_capture(&image)==-EBUSY && !image.size);
 assert(camera_service_capture_pair(&image,&late)==-EBUSY);
 int prior_opens=opens,prior_closes=closes;
 assert(camera_service_capture_paused(&image)==0 && image.size==8);
 assert(opens==prior_opens+1 && closes==prior_closes+1 && !driver_buffers_live);
 assert(g_camera.paused && !g_camera.busy && !g_camera.wanted);
 assert(camera_service_capture(&image)==-EBUSY && !image.size);
 assert(camera_service_control("resume",text,sizeof(text))==0);wait_ready();
 assert(camera_service_capture_paused(&image)==-EBUSY && !image.size);
 assert(camera_service_capture(&image)==0 && image.size==8);
 assert(camera_service_control("resume",text,sizeof(text))==-EINVAL);
 /* Explicit stop during voice must not be undone by its later cleanup. */
 assert(camera_service_control("pause",text,sizeof(text))==0);
 assert(camera_service_control("stop",text,sizeof(text))==0);
 assert(camera_service_control("resume",text,sizeof(text))==0);
 assert(!driver_buffers_live && !g_camera.wanted && !g_camera.paused);
 /* An initially idle camera remains idle after a voice-only session. */
 assert(camera_service_control("pause",text,sizeof(text))==0);
 assert(camera_service_control("resume",text,sizeof(text))==0);
 assert(!driver_buffers_live && !g_camera.wanted);
 /* Live borrowing of an idle camera restores that idle intent, too. */
 assert(camera_service_control("borrow",text,sizeof(text))==0);wait_ready();
 assert(camera_service_capture_live(&image,500,NULL,NULL)==0);
 assert(camera_service_control("resume",text,sizeof(text))==0);
 assert(!driver_buffers_live && !g_camera.wanted && !g_camera.reserved);
 assert(camera_service_control("stop",text,sizeof(text))==0);
 int closed=closes;
 /* A genuine missing stream still closes at the original2s watchdog;
  * delayed poll recovery must not disable failure containment. */
 atomic_store(&readable,false);atomic_store(&never_poll,true);
 assert(camera_service_control("start",text,sizeof(text))==0);
 wait_disabled();
 nxmutex_lock(&g_camera.lock);assert(g_camera.error==-ETIMEDOUT && !g_camera.ready && !g_camera.jpeg);nxmutex_unlock(&g_camera.lock);
 assert(closes==closed+1 && !driver_buffers_live);
 atomic_store(&never_poll,false);closed=closes;
 puts("CAMERA_NO_FRAME_TIMEOUT_PASS original watchdog and cleanup retained; device mocked");
 fail_at=107;assert(camera_service_control("start",text,sizeof(text))==0);
 wait_disabled();assert(closes==closed+1 && !driver_buffers_live);
 nxmutex_lock(&g_camera.lock);assert(g_camera.error==-ENOMEM && !g_camera.jpeg);nxmutex_unlock(&g_camera.lock);
 fail_at=0;assert(camera_service_control("start",text,sizeof(text))==0);wait_ready();
 fail_at=VIDIOC_QBUF;wait_disabled();
 nxmutex_lock(&g_camera.lock);assert(g_camera.error==-EIO && !g_camera.ready);nxmutex_unlock(&g_camera.lock);
 fail_at=0;assert(camera_service_control("start",text,sizeof(text))==0);wait_ready();
 never_poll=true;wait_disabled();assert(!driver_buffers_live);
 nxmutex_lock(&g_camera.lock);assert(g_camera.error==-ETIMEDOUT);nxmutex_unlock(&g_camera.lock);
 never_poll=false;assert(camera_service_control("start",text,sizeof(text))==0);wait_ready();
 fail_at=VIDIOC_STREAMOFF;
 assert(camera_service_control("pause",text,sizeof(text))==-EIO);
 assert(!g_camera.paused && !g_camera.resume_wanted);
 assert(camera_service_control("start",text,sizeof(text))==-EIO);
 nxmutex_lock(&g_camera.lock);assert(g_camera.blocked && !g_camera.wanted && !g_camera.jpeg);nxmutex_unlock(&g_camera.lock);
 int blocked_opens=opens;
 g_camera.reserved=true;
 assert(camera_service_capture_live(&image,2000,NULL,NULL)==-EIO && !image.size && opens==blocked_opens);
 g_camera.reserved=false;
 assert(unmaps==0); /* R1 flat capture buffers are freed by last close. */
 /* Isolated failed cold acquisition while the voice pause is owned. */
 nxmutex_lock(&g_camera.lock);
 g_camera.paused=true;g_camera.blocked=false;g_camera.error=0;g_camera.resume_wanted=true;
 nxmutex_unlock(&g_camera.lock);
 fail_at=100;
 assert(camera_service_capture_paused(&image)==-EIO && !image.size);
 assert(g_camera.blocked && g_camera.paused && !g_camera.resume_wanted);
 fail_at=0;assert(camera_service_control("resume",text,sizeof(text))==0);
 assert(camera_service_control("start",text,sizeof(text))==-EIO);
 puts("PASS: real warm worker, fresh snapshots, pause-owned cold capture, ordinary capture blocked, failed capture quarantine; hardware mocked");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="r1-camera-") as directory:
    path = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    path.write_text(header + source + main)
    for defines in ([], ["-DCONFIG_BK7258_AIDK_CAMERA=1", "-DCONFIG_BUILD_FLAT=1"]):
        subprocess.run([os.environ.get("CC", "cc"), "-D_DEFAULT_SOURCE", "-std=c11",
                    "-Wall", "-Wextra", "-Werror", "-g", "-O1", "-no-pie",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    *defines, str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    path.write_text(header + source + warm_main)
    subprocess.run([os.environ.get("CC", "cc"), "-D_DEFAULT_SOURCE", "-std=c11",
                   "-DCONFIG_BK7258_AIDK_CAMERA=1", "-DCONFIG_BUILD_FLAT=1", "-DCONFIG_BK7258_AP_CORE=1",
                   "-Wall", "-Wextra", "-Werror", "-g", "-O1", "-no-pie", "-pthread",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=60)
