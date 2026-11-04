/* SPDX-License-Identifier: Apache-2.0 */
/* Real UDP sockets, actual mbedTLS HMAC and backend with bounded fake clock. */
#include <assert.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
static uint64_t now=10000;
static int peer=-1,send_mode,open_mode,stops,allocations;
static int fake_clock(clockid_t id,struct timespec *ts)
{assert(id==CLOCK_MONOTONIC);ts->tv_sec=now/1000;ts->tv_nsec=now%1000*1000000;return 0;}
static ssize_t fake_send(int fd,const void *data,size_t bytes,int flags)
{if(send_mode){errno=EAGAIN;return -1;}return send(fd,data,bytes,flags);}
static void *tracked_calloc(size_t count,size_t size);
static void tracked_free(void *p);
#define clock_gettime fake_clock
#define send fake_send
#define calloc tracked_calloc
#define free tracked_free
#include "../../app/vision_badge/src/wake_asr_udp.c"
#undef clock_gettime
#undef send
#undef calloc
#undef free
static void *tracked_calloc(size_t count,size_t size)
{assert(count==1 && size==sizeof(struct wake_backend_s));void *p=calloc(count,size);if(p)allocations++;return p;}
static void tracked_free(void *p)
{const unsigned char *bytes=p;for(size_t i=0;i<sizeof(struct wake_backend_s);i++)assert(!bytes[i]);allocations--;free(p);}
int agora_control_load(cJSON **local){*local=cJSON_CreateObject();return 0;}
int agora_control_wake_udp_target(const cJSON *local,struct sockaddr_in *address,unsigned char key[32])
{
  assert(local);peer=socket(AF_INET,SOCK_DGRAM,0);assert(peer>=0);
  *address=(struct sockaddr_in){.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
  assert(!bind(peer,(struct sockaddr*)address,sizeof(*address)));
  socklen_t length=sizeof(*address);assert(!getsockname(peer,(struct sockaddr*)address,&length));
  assert(fcntl(peer,F_SETFL,O_NONBLOCK)==0);memset(key,0xaa,32);return 0;
}
int agora_control_wake(const cJSON *local,const char *operation,const void *body,size_t size,const char *sequence,cJSON **reply)
{
  assert(local && body && size && !sequence);
  if(!strcmp(operation,"stop")){stops++;*reply=cJSON_CreateObject();return 0;}
  assert(!strcmp(operation,"udp-start"));
  struct sockaddr_in address;socklen_t length=sizeof(address);
  assert(!getsockname(peer,(struct sockaddr*)&address,&length));
  *reply=cJSON_CreateObject();cJSON_AddStringToObject(*reply,"stream","0123456789abcdef0123456789abcdef");
  cJSON_AddNumberToObject(*reply,"protocol",open_mode?1:3);
  cJSON_AddNumberToObject(*reply,"udp_port",ntohs(address.sin_port));return 0;
}
static struct wake_backend_s *open_backend(void)
{
  struct wake_backend_s *b=NULL;now=10000;send_mode=open_mode=0;
  assert(!wake_backend_open(false,&b) && b && allocations==1);return b;
}
static int frame_at(struct wake_backend_s *b,unsigned int index,uint64_t captured,bool *inferred)
{
  int16_t pcm[320];for(unsigned int i=0;i<320;i++)pcm[i]=i+index;
  float score=99;int ret=wake_backend_feed(b,pcm,captured,&score,inferred);
  assert(score==0);return ret;
}
static int frame(struct wake_backend_s *b,unsigned int index,bool *inferred)
{return frame_at(b,index,10000+(uint64_t)index*20,inferred);}
static void packet(struct wake_backend_s *b,unsigned int index)
{
  uint8_t raw[1321];ssize_t count=recv(peer,raw,sizeof(raw),0);assert(count==1320);
  assert(!memcmp(raw,"R1U1",4) && !memcmp(raw+4,b->nonce,16) && udp_u32(raw+20)==index);
  uint8_t tag[32];assert(!udp_tag(b,raw,1304,tag));assert(!memcmp(tag,raw+1304,16));
  if(!index)
    {
      const uint8_t python_tag[]={0x55,0x1c,0x35,0xbb,0xe6,0xdd,0xc0,0xef,0xa9,0x1e,0x24,0x7c,0x76,0x4b,0x88,0x33};
      assert(!memcmp(tag,python_tag,16));
      for(unsigned i=24;i<664;i++)assert(!raw[i]);
    }
  int16_t samples[320];memcpy(samples,raw+664,640);
  for(unsigned i=0;i<320;i++)assert(samples[i]==(int16_t)(index+i));
  if(index)
    {
      memcpy(samples,raw+24,640);
      for(unsigned i=0;i<320;i++)assert(samples[i]==(int16_t)(index-1+i));
    }
}
static void ack(struct wake_backend_s *b,uint32_t sequence,bool positive,bool corrupt)
{
  uint8_t raw[41],tag[32];memcpy(raw,"R1K1",4);memcpy(raw+4,b->nonce,16);
  raw[20]=sequence>>24;raw[21]=sequence>>16;raw[22]=sequence>>8;raw[23]=sequence;raw[24]=positive;
  assert(!udp_tag(b,raw,25,tag));memcpy(raw+25,tag,16);if(corrupt)raw[40]^=1;
  struct sockaddr_in target;socklen_t length=sizeof(target);assert(!getsockname(b->fd,(struct sockaddr*)&target,&length));
  target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(sendto(peer,raw,sizeof(raw),0,(struct sockaddr*)&target,sizeof(target))==(ssize_t)sizeof(raw));
}
static void finish(struct wake_backend_s *b)
{wake_backend_close(b);assert(!allocations);assert(!close(peer));peer=-1;}
int main(void)
{
  bool inferred=false;struct wake_backend_s *b=open_backend();
  for(unsigned i=0;i<25;i++){now=10000+i*20;assert(!frame(b,i,&inferred));packet(b,i);}
  ack(b,24,true,true);assert(!udp_receive(b,&inferred) && !inferred);
  ack(b,25,true,false);assert(!udp_receive(b,&inferred) && !inferred); /* unsent */
  ack(b,24,true,false);assert(udp_receive(b,&inferred)==1 && inferred);
  inferred=false;ack(b,24,true,false);assert(!udp_receive(b,&inferred) && !inferred);
  finish(b);
  b=open_backend();assert(!frame(b,0,&inferred));packet(b,0);
  now=11501;ack(b,0,true,false);inferred=false;
  assert(!udp_receive(b,&inferred) && !inferred && b->last_reply_ms==10000);
  finish(b);
  b=open_backend();send_mode=1;assert(!frame(b,0,&inferred) && b->dropped==1 && !b->has_sent);
  send_mode=0;now+=20;assert(!frame(b,1,&inferred));packet(b,1); /* recovers unsent previous */
  now=13001;assert(frame(b,2,&inferred)==-ETIMEDOUT);finish(b);
  b=open_backend();assert(!frame(b,0,&inferred));packet(b,0);
  assert(frame_at(b,1,9999,&inferred)==-EPIPE);
  assert(frame_at(b,1,10001,&inferred)==-EINVAL);
  wake_backend_gap(b);assert(frame(b,1,&inferred)==-EPIPE);finish(b);
  /* A later datagram covers only itself and its preceding frame. */
  b=open_backend();send_mode=1;assert(!frame(b,0,&inferred));
  now+=20;assert(!frame(b,1,&inferred));
  send_mode=0;now+=20;assert(!frame(b,2,&inferred));packet(b,2);
  ack(b,0,true,false);inferred=false;assert(!udp_receive(b,&inferred) && !inferred);
  ack(b,1,true,false);inferred=false;assert(udp_receive(b,&inferred)==1 && inferred);finish(b);
  /* Equal callback timestamps are valid for a batched read. */
  b=open_backend();assert(!frame_at(b,0,now,&inferred));packet(b,0);
  assert(!frame_at(b,1,now,&inferred));packet(b,1);
  ack(b,1,false,false);inferred=false;assert(!udp_receive(b,&inferred) && inferred);finish(b);
  /* Ten minutes of 1000-ppm sample/system-clock disagreement. The former
   * sequence*20 dating eventually placed every ACK in the future. */
  b=open_backend();
  for(unsigned i=0;i<30000;i++) {
    now=10000+(uint64_t)i*20-i/50;
    assert(!frame_at(b,i,now,&inferred));packet(b,i);
    if(i%25==24) {
      ack(b,i,false,false);inferred=false;
      assert(!udp_receive(b,&inferred) && inferred && b->last_reply_ms==now);
    }
  }
  assert(b->sample_ahead_max_ms>=500 && !b->stale_acks);
  finish(b);
  b=NULL;open_mode=1;assert(wake_backend_open(false,&b)==-EPROTO && !b && !allocations);
  assert(!close(peer));assert(stops==8);
  puts("WAKE_UDP_CAPTURE_TIME_PASS 10 simulated minutes/1000-ppm drift/equal callbacks/backward/future/unsent ACK");
  puts("WAKE_UDP_ADAPTER_PASS native-datagrams/python-HMAC-vector/redundancy/auth/freshness/watchdog/zeroized-release");
  return 0;
}
