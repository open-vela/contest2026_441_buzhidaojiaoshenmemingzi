/* SPDX-License-Identifier: Apache-2.0 */
/* Explicit wake listening only. No PCM persistence, network or SDK call in
 * capture callbacks, and no competing Audio/Camera resource owner. */
#include <nuttx/config.h>
#include <nuttx/kthread.h>
#include <nuttx/mutex.h>
#include <nuttx/signal.h>
#include <arch/chip/bk7258_psram.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <vision_badge/agora_service.h>
#include <vision_badge/services.h>
#include <vision_badge/wake_service.h>
#include <vision_badge/wake_backend.h>
#include <vision_badge/wake_policy.h>

#define WAKE_FRAMES R1_WAKE_QUEUE_FRAMES
struct wake_frame_s
{
  int16_t pcm[320];
  uint64_t end_ms;
#ifdef CONFIG_CONTEST2026_441_WAKE_UDP
  uint64_t captured_ms;
#endif
};
struct wake_queue_s
{
  struct wake_frame_s frames[WAKE_FRAMES];
  int16_t partial[320];
  unsigned int head, count, partial_count;
  uint64_t end_ms, last_arrival;
};
static struct
{
  mutex_t lock;
  bool enabled, busy, stop_capture, retained, diagnostic, local_test;
  unsigned int pauses, generation, windows, dropped, triggers, gaps;
  unsigned int raw_peak, raw_mean, score_milli;
  unsigned int last_score_milli, trace_left;
  unsigned int threshold_percent;
  uint64_t samples, infer_max_ms, frontend_max_ms;
#if defined(CONFIG_CONTEST2026_441_PRODUCT_STARTUP) && defined(CONFIG_CONTEST2026_441_WAKE_ASR)
  unsigned int retry_level, retries;
  uint64_t retry_at_ms;
  int retry_error;
#endif
  int worker_pid, error, capture_result;
  struct wake_queue_s *queue;
  struct wake_backend_s *retained_model;
} w = {.lock=NXMUTEX_INITIALIZER, .threshold_percent=85, .worker_pid=-1};

static uint64_t wake_now_ms(void)
{
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
  return (uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
}
#if defined(CONFIG_CONTEST2026_441_PRODUCT_STARTUP) && defined(CONFIG_CONTEST2026_441_WAKE_ASR)
/* Only transport failures are recoverable. Configuration/model errors,
 * local one-shot diagnostics and device/teardown faults stay fail-closed.
 * This reconnects local ASR, never an RTC conversation. */
static bool wake_retryable(int error)
{
  return error==-ETIMEDOUT || error==-ECONNRESET || error==-ECONNREFUSED ||
         error==-ENETDOWN || error==-ENETUNREACH || error==-EHOSTUNREACH || error==-EPIPE ||
         error==-ENOBUFS || error==-EAGAIN;
}
static unsigned int wake_retry_delay(unsigned int level)
{
  static const unsigned int delays[]={2000,4000,8000,16000,30000};
  return delays[level<5 ? level:4];
}
#endif
static bool wake_cancel(void *unused)
{
  (void)unused;
  nxmutex_lock(&w.lock);
  bool cancel = !w.enabled || w.pauses || w.stop_capture;
  nxmutex_unlock(&w.lock);
  return cancel || agora_service_wake_cancelled();
}
static int wake_pcm(void *unused, const int16_t *pcm, size_t samples)
{
  (void)unused;
  if (!pcm || !samples) return -EINVAL;
  uint64_t arrival=wake_now_ms();
  nxmutex_lock(&w.lock);
  struct wake_queue_s *q=w.queue;
  if (!q || w.stop_capture) { nxmutex_unlock(&w.lock); return -ECANCELED; }
  if (q->last_arrival && arrival-q->last_arrival>200)
    {
      w.dropped+=q->count; q->count=q->partial_count=0; w.generation++; w.gaps++;
      if (arrival>q->end_ms) q->end_ms=arrival;
    }
  if (!q->end_ms) q->end_ms=arrival;
  q->last_arrival=arrival; w.samples+=samples;
  while (samples)
    {
      size_t take=320-q->partial_count;
      if (take>samples) take=samples;
      memcpy(q->partial+q->partial_count,pcm,take*sizeof(int16_t));
      q->partial_count+=take; pcm+=take; samples-=take;
      if (q->partial_count<320) continue;
      unsigned int peak=0, total=0;
      for (unsigned int i=0;i<320;i++)
        {
          int sample=q->partial[i];
          unsigned int magnitude=sample<0?(unsigned int)-sample:(unsigned int)sample;
          if (magnitude>peak) peak=magnitude;
          total+=magnitude;
        }
      if (peak>w.raw_peak) w.raw_peak=peak;
      if (total/320>w.raw_mean) w.raw_mean=total/320;
      if (q->count==WAKE_FRAMES)
        { w.dropped+=q->count; q->count=0; w.generation++; w.gaps++; }
      unsigned int index=(q->head+q->count)%WAKE_FRAMES;
      memcpy(q->frames[index].pcm,q->partial,sizeof(q->partial));
      q->end_ms+=20; q->frames[index].end_ms=q->end_ms;
#ifdef CONFIG_CONTEST2026_441_WAKE_UDP
      /* Sample time and the scheduler's monotonic clock are independent.
       * Keep the 20-ms waveform grid, but date freshness with this actual
       * capture callback, not a grid which accumulates oscillator drift.
       */
      q->frames[index].captured_ms=arrival;
#endif
      q->count++; q->partial_count=0;
    }
  nxmutex_unlock(&w.lock);
  return 0;
}
static void *wake_capture(void *unused)
{
  (void)unused;
  int ret=audio_service_capture_stream(wake_pcm,wake_cancel,NULL);
  nxmutex_lock(&w.lock);
  w.capture_result=ret; w.stop_capture=true;
  nxmutex_unlock(&w.lock);
  return NULL;
}
static int wake_start_capture(pthread_t *thread)
{
  pthread_attr_t attr;
  int ret=pthread_attr_init(&attr);
  if (ret) return -ret;
  ret=pthread_attr_setstacksize(&attr,8192);
  if (!ret) ret=pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
  if (!ret) ret=pthread_attr_setschedpolicy(&attr,SCHED_RR);
  struct sched_param priority={.sched_priority=115};
  if (!ret) ret=pthread_attr_setschedparam(&attr,&priority);
#ifdef CONFIG_SMP
  if (!ret)
    {
      cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(0,&cpus);
      ret=pthread_attr_setaffinity_np(&attr,sizeof(cpus),&cpus);
    }
#endif
  if (!ret) ret=pthread_create(thread,&attr,wake_capture,NULL);
  pthread_attr_destroy(&attr);
  return ret ? -ret : 0;
}
static int wake_worker(int argc, char **argv)
{
  (void)argc; (void)argv;
#ifdef CONFIG_SMP
  /* Same proven timing domain as the existing Agora workers. The second
   * AP core does not yet have independent SysTick acceptance evidence. */
  cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(0,&cpus);
  if (sched_setaffinity(0,sizeof(cpus),&cpus)<0)
    {
      int error=-errno;
      nxmutex_lock(&w.lock);
      w.error=error; w.enabled=false; w.busy=false; w.worker_pid=-1;
      nxmutex_unlock(&w.lock);
      return error;
    }
#endif
  for (;;)
    {
      nxmutex_lock(&w.lock);
      bool ready=w.enabled && !w.pauses && !w.retained;
#if defined(CONFIG_CONTEST2026_441_PRODUCT_STARTUP) && defined(CONFIG_CONTEST2026_441_WAKE_ASR)
      if (ready && w.retry_at_ms>wake_now_ms()) ready=false;
#endif
      if (ready) w.busy=true;
      nxmutex_unlock(&w.lock);
      if (!ready) { nxsig_usleep(20000); continue; }
      int ret=agora_service_wake_acquire();
      if (ret<0)
        {
          nxmutex_lock(&w.lock); w.busy=false; nxmutex_unlock(&w.lock);
          nxsig_usleep(50000); continue;
        }
      struct wake_backend_s *model=NULL;
      bool camera_owned=false, capture_started=false, trigger=false;
      pthread_t capture;
      nxmutex_lock(&w.lock);
      bool require_release=w.triggers!=0;
      bool local_test=w.local_test;
      unsigned int threshold=w.threshold_percent;
      w.stop_capture=false; w.capture_result=0; w.error=0;
      nxmutex_unlock(&w.lock);
#if defined(CONFIG_CONTEST2026_441_PRODUCT_STARTUP) && defined(CONFIG_CONTEST2026_441_WAKE_ASR)
      uint64_t attempt_ms=wake_now_ms();
#endif
      ret=wake_backend_open(require_release,&model);
#ifndef CONFIG_CONTEST2026_441_WAKE_ASR
      if (!ret) ret=r1_wake_model_set_threshold(model,threshold);
      if (!ret) syslog(LOG_INFO,"R1 WAKE model ready arena_used=%lu; acoustic candidate\n",
                       (unsigned long)r1_wake_model_arena_used(model));
#else
      (void)threshold;
      if (!ret) syslog(LOG_INFO,"R1 WAKE backend=lan-asr ready; local computer required\n");
#endif
      struct wake_queue_s *queue=NULL;
      int device_error=0;
      char detail[160];
      if (!ret)
        {
          queue=bk7258_psram_malloc(sizeof(*queue));
          if (!queue) ret=-ENOMEM;
          else memset(queue,0,sizeof(*queue));
        }
      if (!ret && !wake_cancel(NULL))
        {
          ret=camera_service_control("pause",detail,sizeof(detail));
          if (ret<0) device_error=ret; else camera_owned=true;
        }
      if (!ret && camera_owned && !wake_cancel(NULL))
        {
          nxmutex_lock(&w.lock); w.queue=queue; nxmutex_unlock(&w.lock);
          ret=wake_start_capture(&capture); capture_started=ret==0;
        }
      unsigned int generation=0;
      nxmutex_lock(&w.lock); generation=w.generation; nxmutex_unlock(&w.lock);
      while (!ret && capture_started && !wake_cancel(NULL))
        {
          struct wake_frame_s frame;
          nxmutex_lock(&w.lock);
          bool gap=generation!=w.generation;
          generation=w.generation;
          bool available=queue->count!=0;
          if (available) { frame=queue->frames[queue->head]; queue->head=(queue->head+1)%WAKE_FRAMES; queue->count--; }
          nxmutex_unlock(&w.lock);
          if (gap) wake_backend_gap(model);
          if (!available) { nxsig_usleep(5000); continue; }
          float score=0; bool inferred=false;
          uint64_t started=wake_now_ms();
#ifdef CONFIG_CONTEST2026_441_WAKE_UDP
          uint64_t captured=frame.captured_ms;
#else
          uint64_t captured=frame.end_ms;
#endif
          ret=wake_backend_feed(model,frame.pcm,captured,&score,&inferred);
          uint64_t finished=wake_now_ms(), elapsed=finished-started;
#ifndef CONFIG_CONTEST2026_441_WAKE_ASR
          struct r1_wake_model_diag_s diagnostic={0};
          if (inferred) (void)r1_wake_model_diagnostics(model,&diagnostic);
#endif
          nxmutex_lock(&w.lock);
          bool stale=inferred && finished>captured &&
                     finished-captured>R1_WAKE_SCORE_MAX_AGE_MS;
          if (stale && generation==w.generation) { w.generation++; w.gaps++; }
          if (inferred)
            {
              w.windows++; if (elapsed>w.infer_max_ms) w.infer_max_ms=elapsed;
#if defined(CONFIG_CONTEST2026_441_PRODUCT_STARTUP) && defined(CONFIG_CONTEST2026_441_WAKE_ASR)
              /* A fresh acknowledged stream can reset backoff after30s.
               * Merely opening the transport must not reset an outage loop. */
              if (finished>=attempt_ms+30000) w.retry_level=0;
#endif
#ifndef CONFIG_CONTEST2026_441_WAKE_ASR
              if (score>=0 && score<=1)
                { unsigned int milli=(unsigned int)(score*1000+.5f); w.last_score_milli=milli; if (milli>w.score_milli) w.score_milli=milli; }
#endif
            }
          else if (elapsed>w.frontend_max_ms) w.frontend_max_ms=elapsed;
          bool current=!stale && generation==w.generation && w.enabled && !w.pauses && !w.stop_capture;
          if (ret==1 && current) { trigger=true; w.triggers++; }
          bool trace=inferred && w.trace_left!=0;
          if (trace) w.trace_left--;
          nxmutex_unlock(&w.lock);
          if (trace)
            {
#ifdef CONFIG_CONTEST2026_441_WAKE_ASR
              syslog(LOG_INFO,"R1 WAKE ASR "
#if defined(CONFIG_CONTEST2026_441_WAKE_STREAM) || defined(CONFIG_CONTEST2026_441_WAKE_UDP)
                     "stream poll"
#else
                     "chunk"
#endif
                     " cost_ms=%llu stale=%d matched=%d\n",
                     (unsigned long long)elapsed,stale,ret==1);
#else
              unsigned int peak=0;
              for (unsigned i=0;i<320;i++)
                { int value=frame.pcm[i]; unsigned int mag=value<0?(unsigned int)-value:(unsigned int)value; if(mag>peak)peak=mag; }
              syslog(LOG_INFO,"R1 WAKE trace end=%llu scores_milli=%u/%u/%u input_low/high=%u/%u pcm_peak=%u cost_ms=%llu stale=%d\n",
                (unsigned long long)frame.end_ms,diagnostic.scores_milli[0],diagnostic.scores_milli[1],diagnostic.scores_milli[2],
                diagnostic.input_low,diagnostic.input_high,peak,(unsigned long long)elapsed,stale);
#endif
            }
          if (ret==1) { ret=0; if (trigger) break; wake_backend_gap(model); }
        }
      nxmutex_lock(&w.lock); w.stop_capture=true; nxmutex_unlock(&w.lock);
      if (capture_started)
        {
          int joined=pthread_join(capture,NULL);
          if (joined)
            {
              /* The callback may still hold queue/model state. Retain every
               * owner and block voice/power instead of freeing live memory. */
              nxmutex_lock(&w.lock); w.error=-joined; w.retained=true; w.enabled=false; w.retained_model=model; nxmutex_unlock(&w.lock);
              agora_service_wake_release(-joined);
              syslog(LOG_ERR,"R1 WAKE join failed; reset required\n");
              return -joined;
            }
          nxmutex_lock(&w.lock); int captured=w.capture_result; nxmutex_unlock(&w.lock);
          if (captured<0 && captured!=-ECANCELED) device_error=captured;
          if (!ret && captured<0 && captured!=-ECANCELED) ret=captured;
        }
      nxmutex_lock(&w.lock); w.queue=NULL; nxmutex_unlock(&w.lock);
      if (camera_owned && !device_error)
        {
          int resumed=camera_service_control("resume",detail,sizeof(detail));
          if (resumed<0) device_error=resumed;
        }
      if (queue) { memset(queue,0,sizeof(*queue)); bk7258_psram_free(queue); }
      wake_backend_close(model);
      agora_service_wake_release(device_error);
      nxmutex_lock(&w.lock);
      unsigned int retry_delay=0;
#if defined(CONFIG_CONTEST2026_441_PRODUCT_STARTUP) && defined(CONFIG_CONTEST2026_441_WAKE_ASR)
      if (ret<0 && !device_error && !local_test && w.enabled && wake_retryable(ret))
        {
          retry_delay=wake_retry_delay(w.retry_level);
          if (w.retry_level<4) w.retry_level++;
          w.retries++; w.retry_error=ret; w.error=ret;
          w.retry_at_ms=wake_now_ms()+retry_delay;
        }
      else
#endif
      if (ret<0 || device_error<0) { w.error=device_error<0?device_error:ret; w.enabled=false; }
      if (device_error<0) w.retained=true;
      w.busy=false;
      bool request=trigger && w.enabled && !w.pauses && !device_error;
      if (request && local_test) w.enabled=false; /* One local detection only. */
      nxmutex_unlock(&w.lock);
      if (retry_delay)
        syslog(LOG_WARNING,"R1 WAKE local transport retry error=%d after_ms=%u; Audio/Camera owners released, no RTC start\n",ret,retry_delay);
      if (request && local_test)
        syslog(LOG_INFO,"R1 WAKE local detection complete; no voice start\n");
      else if (request && !agora_service_wake_cancelled())
        {
          /* The lease has been released, so cancellation now means a voice
           * start/power transition won the race. The normal start admission
           * decides; never toggle an already-running conversation. */
          ret=agora_service_command("start",detail,sizeof(detail));
          syslog(LOG_INFO,"R1 WAKE voice start queued result=%d\n",ret);
        }
      nxsig_usleep(20000);
    }
}

int wake_service_rpc_begin(bool exclusive)
{
  if (!exclusive) return 0;
  nxmutex_lock(&w.lock); w.pauses++; nxmutex_unlock(&w.lock);
  uint64_t deadline=wake_now_ms()+5000;
  for (;;)
    {
      nxmutex_lock(&w.lock); bool busy=w.busy, retained=w.retained; nxmutex_unlock(&w.lock);
      if (retained) return -EIO;
      if (!busy) return 0;
      if (wake_now_ms()>=deadline) return -ETIMEDOUT;
      nxsig_usleep(10000);
    }
}
void wake_service_rpc_end(bool exclusive)
{
  if (!exclusive) return;
  nxmutex_lock(&w.lock); if (w.pauses) w.pauses--; nxmutex_unlock(&w.lock);
}
int wake_service_command(const char *action,char *text,size_t capacity)
{
  if (!action || !text || !capacity) return -EINVAL;
  if (!strncmp(action,"threshold-",10))
    {
#ifdef CONFIG_CONTEST2026_441_WAKE_ASR
      snprintf(text,capacity,"wake backend=lan-asr; acoustic score threshold unsupported");
      return -ENOTSUP;
#else
      unsigned int percent; char extra;
      if (sscanf(action,"threshold-%u%c",&percent,&extra)!=1 || percent<50 || percent>95) return -EINVAL;
      nxmutex_lock(&w.lock);
      int ret=(w.enabled || w.busy || w.retained)?-EBUSY:0;
      if (!ret) w.threshold_percent=percent;
      snprintf(text,capacity,"wake threshold=%u percent ret=%d; RAM only; two consecutive hits and release guard retained",w.threshold_percent,ret);
      nxmutex_unlock(&w.lock);return ret;
#endif
    }
  if (!strcmp(action,"selftest") || !strcmp(action,"frontend-selftest"))
    {
#ifdef CONFIG_CONTEST2026_441_WAKE_ASR
      snprintf(text,capacity,"wake backend=lan-asr; run host ASR fixtures and live text test");
      return -ENOTSUP;
#else
      int output[3]={0};
      unsigned int changed=0,max_delta=0;
      bool frontend=!strcmp(action,"frontend-selftest");
      nxmutex_lock(&w.lock);
      bool allowed=!w.enabled && !w.busy && !w.retained && !w.pauses;
      if (allowed) { w.diagnostic=true; w.busy=true; }
      nxmutex_unlock(&w.lock);
      int ret=allowed?agora_service_wake_acquire():-EBUSY;
      if (!ret)
        { ret=frontend?r1_wake_model_frontend_selftest(&changed,&max_delta):r1_wake_model_selftest(output); agora_service_wake_release(0); }
      if (allowed)
        { nxmutex_lock(&w.lock); w.diagnostic=false; w.busy=false; nxmutex_unlock(&w.lock); }
      if(frontend)
        snprintf(text,capacity,"wake frontend selftest ret=%d changed=%u max_delta=%u; fixed PCM only; no MIC or cloud",ret,changed,max_delta);
      else
        snprintf(text,capacity,"wake model selftest ret=%d output_q=%d/%d/%d; fixed synthetic features only; no MIC or cloud",ret,output[0],output[1],output[2]);
      return ret;
#endif
    }
  if (strcmp(action,"start") && strcmp(action,"test-start") && strcmp(action,"stop") && strcmp(action,"status") &&
      strcmp(action,"trace-on") && strcmp(action,"trace-off")) return -EINVAL;
  int ret=0;
  nxmutex_lock(&w.lock);
  if (!strcmp(action,"stop"))
    {
      w.enabled=false;
#if defined(CONFIG_CONTEST2026_441_PRODUCT_STARTUP) && defined(CONFIG_CONTEST2026_441_WAKE_ASR)
      w.retry_at_ms=0;
#endif
    }
  else if (!strcmp(action,"trace-off")) w.trace_left=0;
  else if (!strcmp(action,"trace-on"))
    {
      if(w.enabled || w.busy || w.retained) ret=-EBUSY;
      /* Explicit bounded metadata trace, long enough for a human reply.
       * Never log or retain microphone PCM. */
      else { w.trace_left=180; w.raw_peak=w.raw_mean=w.score_milli=w.last_score_milli=0; }
    }
  else if (!strcmp(action,"start") || !strcmp(action,"test-start"))
    {
      bool local_test=!strcmp(action,"test-start");
      if (w.diagnostic || ((w.enabled || w.busy) && w.local_test!=local_test)) ret=-EBUSY;
      else if (w.retained) ret=-EIO;
      else
        {
          /* AP RPC runs at 99, key control at 100. They must preempt inference
           * so status/stop cannot wait for the PCM queue to empty. */
          if (w.worker_pid<0) w.worker_pid=kthread_create("r1-wake",95,8192,wake_worker,NULL);
          if (w.worker_pid<0) ret=w.worker_pid;
          else
            {
#if defined(CONFIG_CONTEST2026_441_PRODUCT_STARTUP) && defined(CONFIG_CONTEST2026_441_WAKE_ASR)
              if (!w.enabled) { w.retry_level=0; w.retry_at_ms=0; }
#endif
              w.local_test=local_test; w.enabled=true;
            }
        }
    }
#ifdef CONFIG_CONTEST2026_441_WAKE_ASR
  snprintf(text,capacity,"wake enabled=%d busy=%d retained=%d error=%d "
#ifdef CONFIG_CONTEST2026_441_WAKE_UDP
    "responses=%u dropped=%u gaps=%u triggers=%u poll_max_ms=%llu raw_peak=%u test=%d backend=lan-asr transport=udp",
#elif defined(CONFIG_CONTEST2026_441_WAKE_STREAM)
    "responses=%u dropped=%u gaps=%u triggers=%u poll_max_ms=%llu raw_peak=%u test=%d backend=lan-asr transport=tcp",
#else
    "chunks=%u dropped=%u gaps=%u triggers=%u http_asr_max_ms=%llu raw_peak=%u test=%d backend=lan-asr",
#endif
    w.enabled,w.busy,w.retained,w.error,w.windows,w.dropped,w.gaps,w.triggers,
    (unsigned long long)w.infer_max_ms,w.raw_peak,w.local_test);
#else
  snprintf(text,capacity,"wake enabled=%d busy=%d retained=%d error=%d windows=%u dropped=%u gaps=%u triggers=%u infer_max_ms=%llu front_ms=%llu raw_peak=%u raw_mean=%u score_milli=%u last_score=%u test=%d",
    w.enabled,w.busy,w.retained,w.error,w.windows,w.dropped,w.gaps,w.triggers,(unsigned long long)w.infer_max_ms,
    (unsigned long long)w.frontend_max_ms,w.raw_peak,w.raw_mean,w.score_milli,w.last_score_milli,w.local_test);
#endif
#if defined(CONFIG_CONTEST2026_441_PRODUCT_STARTUP) && defined(CONFIG_CONTEST2026_441_WAKE_ASR)
  size_t used=strlen(text);
  uint64_t retry_now=wake_now_ms();
  if (used<capacity) snprintf(text+used,capacity-used," retries=%u wait_ms=%u last=%d",
      w.retries,w.retry_at_ms>retry_now?(unsigned int)(w.retry_at_ms-retry_now):0,w.retry_error);
#endif
  nxmutex_unlock(&w.lock);
  return ret;
}
