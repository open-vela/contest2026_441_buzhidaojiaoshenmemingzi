/* SPDX-License-Identifier: Apache-2.0 */
/* Actual wake service; scheduler/ML scores/Audio/Camera/Agora are substitutes. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vision_badge/wake_model.h>
#ifdef CONFIG_SMP
static atomic_int pin_calls, capture_pin_calls;
static int pin_error;
static int attr_affinity(pthread_attr_t *a,size_t size,const cpu_set_t *cpus)
{ (void)a;assert(size==sizeof(*cpus) && CPU_COUNT(cpus)==1 && CPU_ISSET(0,cpus));capture_pin_calls++;return 0; }
static int worker_affinity(pid_t pid,size_t size,const cpu_set_t *cpus)
{ assert(pid==0 && size==sizeof(*cpus) && CPU_COUNT(cpus)==1 && CPU_ISSET(0,cpus));pin_calls++;if(pin_error){errno=pin_error;return -1;}return 0; }
#define pthread_attr_setaffinity_np attr_affinity
#define sched_setaffinity worker_affinity
#endif
#define CONFIG_CONTEST2026_441_WAKE 1
static int attr_ok(pthread_attr_t *a, int x) { (void)a;(void)x;return 0; }
static int attr_stack(pthread_attr_t *a,size_t s) { assert(s==8192);(void)a;return 0; }
static int attr_priority(pthread_attr_t *a,const struct sched_param *s) {assert(s->sched_priority==115);(void)a;return 0;}
static int join_wrapper(pthread_t,void**);
#define pthread_attr_setstacksize attr_stack
#define pthread_attr_setinheritsched attr_ok
#define pthread_attr_setschedpolicy attr_ok
#define pthread_attr_setschedparam attr_priority
#define pthread_join join_wrapper
#include "../../app/vision_badge/src/wake_service.c"
#undef pthread_join
#undef pthread_attr_setstacksize
#undef pthread_attr_setinheritsched
#undef pthread_attr_setschedpolicy
#undef pthread_attr_setschedparam
static pthread_t worker_thread;
static int (*worker_entry)(int,char**);
static atomic_bool owner, voice, power_inhibit, capture_active, camera_paused, blocked, feed_active;
static atomic_int allocations, captures, pauses, resumes, voice_calls, feeds, opens;
static atomic_int model_error, model_feed_error;
static int capture_error, resume_error, join_error, mode;
static void *worker_bridge(void *arg) { (void)arg;worker_entry(0,NULL);return NULL; }
int kthread_create(const char *name,int priority,int stack,int (*entry)(int,char**),char *const argv[])
{
  assert(!strcmp(name,"r1-wake") && priority>0 && priority<99 && stack==8192 && !argv);
  worker_entry=entry;assert(!pthread_create(&worker_thread,NULL,worker_bridge,NULL));return 123;
}
int nxsig_usleep(unsigned int us) { usleep(us);return 0; }
void *bk7258_psram_malloc(size_t size) { void *p=malloc(size);if(p)allocations++;return p; }
void bk7258_psram_free(void *p) {if(p){allocations--;free(p);}}
struct r1_wake_model_s { int unused; };
const unsigned char r1_wake_model_data[16]={0}; const size_t r1_wake_model_bytes=16;
int r1_wake_model_open(const unsigned char *d,size_t bytes,bool release,struct r1_wake_model_s **out)
{ assert(d==r1_wake_model_data && bytes==16);(void)release;opens++;if(model_error)return model_error;*out=bk7258_psram_malloc(sizeof(**out));return *out?0:-ENOMEM; }
void r1_wake_model_close(struct r1_wake_model_s *m) {assert(!capture_active);bk7258_psram_free(m);}
void r1_wake_model_gap(struct r1_wake_model_s *m) {assert(m);}
size_t r1_wake_model_arena_used(const struct r1_wake_model_s *m){(void)m;return 100;}
static int selftest_error;
int r1_wake_model_selftest(int q[3]){assert(owner && !capture_active);q[0]=0;q[1]=0;q[2]=127;return selftest_error;}
int r1_wake_model_frontend_selftest(unsigned int *changed,unsigned int *max_delta)
{assert(owner && !capture_active);*changed=*max_delta=0;return selftest_error;}
int r1_wake_model_set_threshold(struct r1_wake_model_s *m,unsigned int p){assert(m && p>=50 && p<=95);return 0;}
int r1_wake_model_diagnostics(const struct r1_wake_model_s *m,struct r1_wake_model_diag_s *d)
{assert(m && d);memset(d,0,sizeof(*d));d->valid=true;d->scores_milli[2]=900;return 0;}
int r1_wake_model_feed(struct r1_wake_model_s *m,const int16_t pcm[320],uint64_t ms,float *score,bool *inferred)
{
  /* An owned queued frame remains valid after capture exits. The worker
   * must retain its lease/model until inference and join have completed. */
  assert(m && pcm && ms && owner);*score=.9f;*inferred=true;
  int count=++feeds;
  if(mode==3){feed_active=true;while(capture_active)usleep(1000);return 1;}
  if(mode==2 && count==1){usleep(50000);return 1;} /* Stale score after producer overflow. */
  if(mode==4 && count==1){feed_active=true;usleep((R1_WAKE_SCORE_MAX_AGE_MS+150u)*1000u);return 1;} /* Old decision, without queue overflow. */
  if(mode==5)
    {
      *inferred=count%25==0;
      if(count==25)usleep(1200000); /* Measured LAN jitter while capture continues. */
      return count==50?1:0;
    }
  if(model_feed_error)return model_feed_error;
  return mode==1 && count==3 ? 1:0;
}
#ifdef CONFIG_CONTEST2026_441_WAKE_ASR
struct wake_backend_s { int unused; };
int wake_backend_open(bool release,struct wake_backend_s **out)
{ return r1_wake_model_open(r1_wake_model_data,16,release,(struct r1_wake_model_s **)out); }
void wake_backend_close(struct wake_backend_s *b) {r1_wake_model_close((struct r1_wake_model_s *)b);}
void wake_backend_gap(struct wake_backend_s *b) {r1_wake_model_gap((struct r1_wake_model_s *)b);}
int wake_backend_feed(struct wake_backend_s *b,const int16_t pcm[320],uint64_t ms,float *score,bool *inferred)
{
#ifdef CONFIG_CONTEST2026_441_WAKE_UDP
  assert(ms<=wake_now_ms()); /* Actual capture clock, never the future PCM grid. */
#endif
  return r1_wake_model_feed((struct r1_wake_model_s *)b,pcm,ms,score,inferred);
}
#endif
int agora_service_wake_acquire(void)
{
  if(voice || power_inhibit || blocked)return -EBUSY;
  bool expected=false;return atomic_compare_exchange_strong(&owner,&expected,true)?0:-EBUSY;
}
bool agora_service_wake_cancelled(void){return voice||power_inhibit||blocked;}
void agora_service_wake_release(int error)
{ if(!join_error)assert(!capture_active);if(error<0)blocked=true;owner=false; }
int agora_service_command(const char *action,char *text,size_t size)
{
  (void)text;(void)size;assert(!strcmp(action,"start") && !owner && !capture_active && !camera_paused);
  voice_calls++;voice=true;return 0;
}
int camera_service_control(const char *action,char *text,size_t size)
{
  (void)text;(void)size;
  if(!strcmp(action,"pause")){assert(owner && !capture_active && !camera_paused);camera_paused=true;pauses++;return 0;}
  assert(!strcmp(action,"resume") && camera_paused && !capture_active);
  resumes++;if(resume_error)return resume_error;camera_paused=false;return 0;
}
int audio_service_capture_stream(audio_capture_cb_t sink,audio_cancel_cb_t cancel,void *arg)
{
  assert(owner && camera_paused && !voice);capture_active=true;captures++;
  int16_t pcm[320]={-32768,32767};int result=capture_error;
  while(!result && !cancel(arg))
    {
      result=sink(arg,pcm,320);
      if(mode==4){while(!cancel(arg))usleep(1000);break;}
      if(mode!=2)usleep(mode==5?20000:1000); /* Jitter case uses actual PCM pacing. */
    }
  capture_active=false;return result;
}
static int join_wrapper(pthread_t thread,void **value)
{ if(join_error)return join_error;return pthread_join(thread,value); }
static bool read_busy(void) {nxmutex_lock(&w.lock);bool b=w.busy;nxmutex_unlock(&w.lock);return b;}
static void wait_flag(atomic_bool *value,bool expected)
{for(unsigned i=0;i<3000 && atomic_load(value)!=expected;i++)usleep(1000);assert(atomic_load(value)==expected);}
static void wait_disabled(void)
{
  for(unsigned i=0;i<3000;i++)
    {nxmutex_lock(&w.lock);bool done=!w.enabled && (!w.busy || w.retained);nxmutex_unlock(&w.lock);if(done)return;usleep(1000);}
  assert(false);
}
static int command(const char *action) {char text[256];return wake_service_command(action,text,sizeof(text));}
#if defined(CONFIG_CONTEST2026_441_PRODUCT_STARTUP) && defined(CONFIG_CONTEST2026_441_WAKE_ASR)
static void wait_retry(unsigned int retries)
{
  bool done=false;
  for(unsigned int i=0;i<3000;i++)
    {
      nxmutex_lock(&w.lock);
      done=w.retries==retries && w.enabled && !w.busy;
      nxmutex_unlock(&w.lock);
      if(done)break;
      usleep(1000);
    }
  assert(done && !owner && !capture_active && !camera_paused && !allocations && !voice_calls);
}
static void retry_now(void)
{ nxmutex_lock(&w.lock);w.retry_at_ms=0;nxmutex_unlock(&w.lock); }
static void recovery_test(const char *scenario)
{
  assert(wake_retry_delay(0)==2000 && wake_retry_delay(1)==4000 &&
         wake_retry_delay(2)==8000 && wake_retry_delay(3)==16000 &&
         wake_retry_delay(4)==30000 && wake_retry_delay(99)==30000);
  for(unsigned i=0;i<6;i++)
    assert(!wake_retryable((int[]){-ENOMEM,-EPROTO,-EACCES,-ECANCELED,-EINVAL,-EIO}[i]));
  bool opening=!strcmp(scenario,"retry-open") || !strcmp(scenario,"retry-backoff") || !strcmp(scenario,"retry-busy");
  int open_error=!strcmp(scenario,"retry-busy")?-EAGAIN:-ECONNREFUSED;
  if(opening) model_error=open_error;
  else model_feed_error=-ETIMEDOUT;
  if(!strcmp(scenario,"retry-capture-timeout"))
    {
      capture_error=-ETIMEDOUT;assert(!command("start"));wait_disabled();
      assert(w.retained && blocked && !w.retries && !voice_calls);
    }
  else if(!strcmp(scenario,"retry-device-fault"))
    {
      resume_error=-EIO;assert(!command("start"));wait_disabled();
      assert(w.retained && blocked && !w.retries && !voice_calls);
    }
  else if(!strcmp(scenario,"retry-local-test"))
    {
      assert(!command("test-start"));wait_disabled();
      assert(!w.retries && !allocations && !camera_paused && !owner);
    }
  else
    {
      assert(!command("start"));wait_retry(1);
      assert(w.retry_level==1 && w.retry_error==(opening?open_error:-ETIMEDOUT));
      char status[256];assert(!wake_service_command("status",status,sizeof(status)));
      assert(strstr(status,"retries=1") && strstr(status,"last="));
      int old_opens=opens;
      usleep(80000);assert(opens==old_opens); /* Not spinning during backoff. */
      if(!strcmp(scenario,"retry-backoff"))
        {
          for(unsigned int i=2;i<=7;i++)
            {
              retry_now();wait_retry(i);
              nxmutex_lock(&w.lock);
              uint64_t now=wake_now_ms(),until=w.retry_at_ms;
              assert(w.retry_level==(i<4?i:4));
              assert(until>now && until-now<=wake_retry_delay(i-1));
              assert(until-now+500>=wake_retry_delay(i-1));
              nxmutex_unlock(&w.lock);
            }
          assert(opens==7 && !command("stop"));
        }
      else if(!strcmp(scenario,"retry-stop"))
        {assert(!command("stop"));retry_now();usleep(80000);assert(opens==old_opens);}
      else if(!strcmp(scenario,"retry-power") || !strcmp(scenario,"retry-manual"))
        {
          if(!strcmp(scenario,"retry-power"))power_inhibit=true;else voice=true;
          retry_now();usleep(100000);assert(opens==old_opens && !allocations && !owner);
          assert(!command("stop"));
        }
      else
        {
          /* RPC pause also blocks an already-due retry. */
          assert(!wake_service_rpc_begin(true));retry_now();
          usleep(80000);assert(opens==old_opens);
          model_error=model_feed_error=0;
          bool then_wake=!strcmp(scenario,"retry-then-wake");
          if(then_wake)mode=1;
          wake_service_rpc_end(true);
          if(then_wake)
            {wait_flag(&voice,true);assert(voice_calls==1 && !owner && !capture_active);}
          else
            {wait_flag(&capture_active,true);assert(!voice_calls);}
          assert(opens==old_opens+1 && w.retry_level==1);
          assert(!command("stop"));wait_disabled();assert(!allocations && !owner && !camera_paused);
        }
    }
  pthread_cancel(worker_thread);pthread_join(worker_thread,NULL);
  printf("WAKE_RECOVERY_PASS %s: real owner release/backoff/stop, transport/devices/matches mocked; no automatic RTC retry\n",scenario);
}
#endif
int main(int argc,char **argv)
{
  assert(argc==2);
  const char *s=argv[1];
#if defined(CONFIG_CONTEST2026_441_PRODUCT_STARTUP) && defined(CONFIG_CONTEST2026_441_WAKE_ASR)
  if(!strncmp(s,"retry-",6)){recovery_test(s);return 0;}
#endif
#ifdef CONFIG_CONTEST2026_441_WAKE_UDP
  if(!strcmp(s,"capture-time"))
    {
      int16_t pcm[640]={0};
      w.queue=bk7258_psram_malloc(sizeof(*w.queue));assert(w.queue);
      memset(w.queue,0,sizeof(*w.queue));
      w.queue->end_ms=wake_now_ms()+60000; /* Deliberately drifting sample clock. */
      assert(!wake_pcm(NULL,pcm,640) && w.queue->count==2);
      assert(w.queue->frames[0].end_ms>wake_now_ms()+59000);
      assert(w.queue->frames[0].captured_ms==w.queue->frames[1].captured_ms);
      assert(w.queue->frames[0].captured_ms<=wake_now_ms());
      bk7258_psram_free(w.queue);w.queue=NULL;assert(!allocations);
      puts("WAKE_CAPTURE_TIME_PASS actual callback dates batched PCM independently of sample grid");return 0;
    }
#endif
  if(!strcmp(s,"selftest") || !strcmp(s,"selftest-fail") || !strcmp(s,"frontend-selftest") || !strcmp(s,"frontend-selftest-fail"))
    {
#ifdef CONFIG_CONTEST2026_441_WAKE_ASR
      assert(command("selftest")==-ENOTSUP && command("frontend-selftest")==-ENOTSUP);
      assert(command("threshold-85")==-ENOTSUP);
      assert(!owner && !w.busy && !captures && !allocations);
      puts("WAKE_ASR_NO_FAKE_MODEL_SELFTEST_PASS");return 0;
#else
      selftest_error=strstr(s,"-fail")?-EBADMSG:0;
      const char *action=!strncmp(s,"frontend-",9)?"frontend-selftest":"selftest";
      assert(command(action)==selftest_error && !owner && !w.busy && !w.diagnostic && !captures && !allocations);
      w.enabled=true;assert(command("selftest")==-EBUSY);w.enabled=false;
      w.pauses=1;assert(command("selftest")==-EBUSY);w.pauses=0;
      power_inhibit=true;assert(command("selftest")==-EBUSY && !w.busy && !w.diagnostic);power_inhibit=false;
      assert(command("trace-on")==0 && w.trace_left==180);
      w.enabled=true;assert(command("trace-on")==-EBUSY);w.enabled=false;
      assert(command("trace-off")==0 && !w.trace_left);
      assert(command("threshold-60")==0 && w.threshold_percent==60);
      w.enabled=true;assert(command("threshold-85")==-EBUSY && w.threshold_percent==60);w.enabled=false;
      assert(command("threshold-49")==-EINVAL && command("threshold-96")==-EINVAL && command("threshold-60x")==-EINVAL);
      assert(command("threshold-85")==0 && w.threshold_percent==85);
      puts("WAKE_SELFTEST_ADMISSION_PASS idle-only, owner released on failure, no Audio/Camera, trace explicit");return 0;
#endif
    }
#ifdef CONFIG_SMP
  if(!strcmp(s,"pin-fail"))pin_error=EIO;
#endif
  if(!strcmp(s,"model-fail"))model_error=-ENOMEM;
  if(!strcmp(s,"capture-fail"))capture_error=-EIO;
  if(!strcmp(s,"resume-fail"))resume_error=-EIO;
  if(!strcmp(s,"feed-fail"))model_feed_error=-EPROTO;
  if(!strcmp(s,"join-fail"))join_error=EDEADLK;
  bool local=!strcmp(s,"local-trigger") || !strcmp(s,"local-stop") || !strcmp(s,"asr-jitter");
  if(!strcmp(s,"trigger") || !strcmp(s,"local-trigger"))mode=1;
  if(!strcmp(s,"overflow"))mode=2;
  if(!strcmp(s,"stale-stop"))mode=3;
  if(!strcmp(s,"stale-age"))mode=4;
  if(!strcmp(s,"asr-jitter"))mode=5;
  assert(command("invalid")==-EINVAL && !captures && !voice_calls);
  assert(command(local?"test-start":"start")==0);
#ifdef CONFIG_SMP
  if(pin_error)
    {wait_disabled();assert(!captures && !pauses && !voice_calls && !allocations && w.error==-EIO);}
  else
#endif
  if(model_error || capture_error || model_feed_error)
    {
      wait_disabled();assert(!voice_calls && !allocations);
      if(model_error)assert(!pauses && !blocked);
      else if(capture_error)assert(blocked && camera_paused && !resumes);
      else assert(!blocked && !camera_paused);
    }
  else if(mode==1 || mode==5)
    {
      if(local) {wait_disabled();assert(!voice_calls && w.triggers==1 && w.local_test);}
      else {wait_flag(&voice,true);assert(voice_calls==1);}
      assert(!capture_active && !owner && !camera_paused);
      if(mode==5)assert(w.dropped==0 && w.gaps==0 && w.infer_max_ms>=1200 && !voice_calls);
      nxmutex_lock(&w.lock);
      assert(w.raw_peak==32768 && w.raw_mean==204);
#ifdef CONFIG_CONTEST2026_441_WAKE_ASR
      assert(w.score_milli==0); /* Text match must not masquerade as confidence. */
#else
      assert(w.score_milli==900);
#endif
      nxmutex_unlock(&w.lock);
      char status[256];assert(!wake_service_command("status",status,sizeof(status)));
      assert(strstr(status,"raw_peak=32768"));
#ifdef CONFIG_CONTEST2026_441_WAKE_ASR
      assert(strstr(status,"backend=lan-asr") && !strstr(status,"score_milli="));
#else
      assert(strstr(status,"score_milli=900"));
#endif
      assert(command("stop")==0);wait_disabled();assert(!allocations);
    }
  else
    {
      wait_flag(&capture_active,true);
      if(local) assert(command("start")==-EBUSY && w.local_test && !voice_calls);
      if(!strcmp(s,"manual")){voice=true;}
      else if(!strcmp(s,"power")){power_inhibit=true;}
      else if(!strcmp(s,"rpc"))
        {
          assert(wake_service_rpc_begin(true)==0 && !read_busy() && !capture_active && !owner);
          assert(!allocations);wake_service_rpc_end(true);
          wait_flag(&capture_active,true);
        }
      else if(mode==2)
        {usleep(120000);assert(!voice_calls);nxmutex_lock(&w.lock);assert(w.dropped && w.gaps && !w.triggers);nxmutex_unlock(&w.lock);}
      else if(mode==3)wait_flag(&feed_active,true);
      else if(mode==4)
        {wait_flag(&feed_active,true);usleep((R1_WAKE_SCORE_MAX_AGE_MS+300u)*1000u);assert(!voice_calls);nxmutex_lock(&w.lock);assert(w.gaps==1 && !w.dropped && !w.triggers);nxmutex_unlock(&w.lock);}
      assert(command("stop")==0);wait_disabled();
      assert(!voice_calls);
      if(resume_error || join_error)assert(blocked && w.retained);
      else assert(!capture_active && !camera_paused && !owner && !allocations);
    }
  /* Only stop the mock permanent kthread after its production state is idle.
   * Real firmware keeps this one worker asleep for the next explicit start. */
  pthread_cancel(worker_thread);pthread_join(worker_thread,NULL);
#ifdef CONFIG_SMP
  assert(pin_calls==1);
  if(pin_error || model_error)assert(capture_pin_calls==0);
  else assert(capture_pin_calls>=1);
#endif
  printf("WAKE_SERVICE_PASS %s: real owner/queue/cleanup, devices/model/scheduler mocked\n",s);
  return 0;
}
