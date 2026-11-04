/* SPDX-License-Identifier: Apache-2.0 */
/* Actual adapter with substituted LAN transport; no Audio/cloud operations. */
#include <assert.h>
#include <stdlib.h>
#include "../../app/vision_badge/src/wake_asr.c"
static int allocations, mode, calls, frames, stops;
static const char *id="0123456789abcdef0123456789abcdef";
void *bk7258_psram_malloc(size_t bytes) {allocations++;return malloc(bytes);}
void bk7258_psram_free(void *p)
{
  const unsigned char *bytes=p;
  for(size_t i=0;i<sizeof(struct wake_backend_s);i++)assert(bytes[i]==0);
  allocations--;free(p);
}
int agora_control_load(cJSON **local) {*local=cJSON_CreateObject();return 0;}
int agora_control_wake(const cJSON *local,const char *op,const void *body,size_t size,
                       const char *sequence,cJSON **reply)
{
  assert(local && body);calls++;
  if(mode==1)return -ETIMEDOUT;
  char response[256];
  if(!strcmp(op,"start"))
    {
      assert(size==2 && !sequence);
      snprintf(response,sizeof(response),"{\"protocol\":%d,\"stream\":\"%s\"}",mode==2?2:1,id);
    }
  else if(!strcmp(op,"frame"))
    {
      assert(size==16000 && sequence && strstr(sequence,id));frames++;
      const int16_t *samples=body;
      for(unsigned i=0;i<8000;i++)assert(samples[i]==(int16_t)(i%320));
      snprintf(response,sizeof(response),"{\"stream\":\"%s\",\"seq\":%u,\"matched\":%s}",
               id,mode==3?999:(unsigned)frames-1,mode==4?"true":"false");
    }
  else {assert(!strcmp(op,"stop") && !sequence);stops++;snprintf(response,sizeof(response),"{\"ok\":true}");}
  *reply=cJSON_Parse(response);return 0;
}
static int feed_chunk(struct wake_backend_s *backend,uint64_t *ms)
{
  int16_t pcm[320];for(unsigned i=0;i<320;i++)pcm[i]=i;
  float score=77;bool inferred=true;
  for(unsigned i=0;i<25;i++)
    {
      *ms+=20;int ret=wake_backend_feed(backend,pcm,*ms,&score,&inferred);
      assert(score==0 && inferred==(i==24));
      if(i==24)return ret;
      assert(ret==0);
    }
  abort();
}
int main(void)
{
  struct wake_backend_s *backend=NULL;
  for(mode=1;mode<=2;mode++)
    {assert(wake_backend_open(false,&backend)<0 && !backend && !allocations);}
  mode=0;uint64_t ms=0;
  assert(wake_backend_open(false,&backend)==0 && allocations==1);
  assert(feed_chunk(backend,&ms)==0 && frames==1);
  mode=4;assert(feed_chunk(backend,&ms)==1 && frames==2);
  wake_backend_close(backend);assert(!allocations && stops==1);
  frames=0;mode=0;ms=0;
  assert(wake_backend_open(true,&backend)==0);
  mode=3;assert(feed_chunk(backend,&ms)==-EPROTO && !backend->valid);
  wake_backend_close(backend);assert(!allocations);
  frames=0;mode=0;ms=0;
  assert(wake_backend_open(false,&backend)==0);
  wake_backend_gap(backend);
  int16_t pcm[320]={0};float score;bool inferred;
  assert(wake_backend_feed(backend,pcm,20,&score,&inferred)==-EPIPE);
  assert(frames==0);wake_backend_close(backend);assert(!allocations);
  assert(wake_backend_open(false,&backend)==0);
  assert(wake_backend_feed(backend,pcm,20,&score,&inferred)==0);
  assert(wake_backend_feed(backend,pcm,60,&score,&inferred)==-EPIPE);
  wake_backend_close(backend);assert(!allocations);
  puts("WAKE_ASR_TRANSPORT_PASS chunks/order/gap/schema/error/zeroized-release; no real ASR claim");
  return 0;
}
