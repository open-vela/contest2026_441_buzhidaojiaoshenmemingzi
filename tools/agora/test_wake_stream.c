/* SPDX-License-Identifier: Apache-2.0 */
/* Real nonblocking socketpair I/O and adapter, substituted tunnel/clock. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
static uint64_t fake_now=10000;
static int peer=-1,allocations,tunnel_mode,send_mode,allocation_fail,send_calls;
static void *backend_malloc(size_t size);
static void backend_free(void *pointer);
static int fake_clock(clockid_t id,struct timespec *ts)
{assert(id==CLOCK_MONOTONIC);ts->tv_sec=fake_now/1000;ts->tv_nsec=(fake_now%1000)*1000000;return 0;}
static int fake_setsockopt(int fd,int level,int name,const void *value,socklen_t bytes)
{assert(fd>=0 && level==IPPROTO_TCP && name==TCP_NODELAY && bytes==sizeof(int) && *(const int*)value==1);return 0;}
static ssize_t fake_send(int fd,const void *data,size_t bytes,int flags)
{
  send_calls++;
  if(send_mode==1){errno=EAGAIN;return -1;}
  if(send_mode==2 && bytes>13)bytes=13;
  if(send_mode==3 && bytes>1)bytes=1;
  return send(fd,data,bytes,flags);
}
#define clock_gettime fake_clock
#define setsockopt fake_setsockopt
#define send fake_send
#define malloc backend_malloc
#define free backend_free
#include "../../app/vision_badge/src/wake_asr_stream.c"
#undef malloc
#undef free
#undef clock_gettime
#undef setsockopt
#undef send
/* The SDK private pool is unavailable: ordinary TCP buffers must still work. */
void *bk7258_psram_malloc(size_t size){(void)size;return NULL;}
void bk7258_psram_free(void *p){(void)p;assert(!"TCP buffer used private SDK heap");}
static void *backend_malloc(size_t size)
{assert(size==sizeof(struct wake_backend_s));if(allocation_fail)return NULL;void *p=malloc(size);if(p)allocations++;return p;}
static void backend_free(void *p)
{
  const unsigned char *bytes=p;for(size_t i=0;i<sizeof(struct wake_backend_s);i++)assert(!bytes[i]);
  allocations--;free(p);
}
int agora_control_load(cJSON **local){*local=cJSON_CreateObject();return 0;}
int agora_control_wake_stream(const cJSON *local,struct webclient_conn_s **conn)
{
  assert(local);*conn=NULL;if(tunnel_mode==1)return -ETIMEDOUT;
  int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));peer=pair[1];
  struct timeval timeout={.tv_sec=1};assert(!setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
  *conn=calloc(1,sizeof(**conn));assert(*conn);(*conn)->sockfd=pair[0];
  const char *hello=tunnel_mode==2?"{\"protocol\":1,\"stream\":\"0123456789abcdef0123456789abcdef\"}\n":
    "{\"protocol\":2,\"stream\":\"0123456789abcdef0123456789abcdef\"}\n";
  assert(write(peer,hello,strlen(hello))==(ssize_t)strlen(hello));return 0;
}
void webclient_conn_close(struct webclient_conn_s *conn){assert(!close(conn->sockfd));}
void webclient_conn_free(struct webclient_conn_s *conn){free(conn);}
static struct wake_backend_s *open_stream(void)
{
  struct wake_backend_s *b=NULL;fake_now=10000;send_mode=tunnel_mode=0;
  assert(!wake_backend_open(false,&b) && b && allocations==1);
  char magic[4];assert(read(peer,magic,4)==4 && !memcmp(magic,"R1A2",4));return b;
}
static int frame(struct wake_backend_s *b,unsigned int index)
{
  int16_t pcm[320];for(unsigned i=0;i<320;i++)pcm[i]=i;
  float score=123;bool inferred=false;
  int ret=wake_backend_feed(b,pcm,10000+(uint64_t)index*20,&score,&inferred);
  assert(score==0);return ret;
}
static void read_frames(unsigned int first,unsigned int count)
{
  uint8_t packet[644];
  for(unsigned int index=0;index<count;index++)
    {
      size_t used=0;while(used<sizeof(packet)){ssize_t bytes=read(peer,packet+used,sizeof(packet)-used);assert(bytes>0);used+=(size_t)bytes;}
      assert((((unsigned)packet[0]<<24)|((unsigned)packet[1]<<16)|((unsigned)packet[2]<<8)|packet[3])==first+index);
      int16_t pcm[320];memcpy(pcm,packet+4,sizeof(pcm));for(unsigned i=0;i<320;i++)assert(pcm[i]==(int16_t)i);
    }
}
static void ack(unsigned int index,bool matched)
{
  char data[200];int size=snprintf(data,sizeof(data),"{\"stream\":\"0123456789abcdef0123456789abcdef\",\"last_frame\":%u,\"matched\":%s}\n",index,matched?"true":"false");
  assert(write(peer,data,7)==7);assert(write(peer,data+7,size-7)==size-7);
}
static void close_stream(struct wake_backend_s *b)
{wake_backend_close(b);assert(!allocations);if(peer>=0){close(peer);peer=-1;}}
int main(void)
{
  struct wake_backend_s *b=NULL;
  /* Runtime allocation also references these wrappers on the old source. */
  (void)backend_malloc;(void)backend_free;
  allocation_fail=1;assert(wake_backend_open(false,&b)==-ENOMEM && !b && !allocations);allocation_fail=0;
  tunnel_mode=1;assert(wake_backend_open(false,&b)==-ETIMEDOUT && !b && !allocations);
  tunnel_mode=2;assert(wake_backend_open(false,&b)==-EPROTO && !b && !allocations);close(peer);peer=-1;
  b=open_stream();
  send_calls=0;
  for(unsigned i=0;i<25;i++){fake_now=10000+i*20;assert(!frame(b,i));}
  assert(send_calls==5 && b->queue_high==5);
  read_frames(0,25);ack(24,true);fake_now=10500;assert(frame(b,25)==1);close_stream(b);
  b=open_stream();assert(!frame(b,0) && b->count==1);
  fake_now=10099;assert(!stream_flush(b) && b->count==1);
  fake_now=10100;assert(!stream_flush(b) && !b->count);read_frames(0,1);close_stream(b);
  b=open_stream();
  for(unsigned i=0;i<25;i++){fake_now=10000+i*20;assert(!frame(b,i));}
  read_frames(0,25);ack(24,true);fake_now=12100;assert(frame(b,25)==0 && !b->matched); /* Old ACK never wakes. */
  for(unsigned i=26;i<106;i++)assert(!frame(b,i));
  ack(104,true);assert(frame(b,106)==1);close_stream(b); /* New speech can still wake. */
  /* Short successful writes are progress, not EAGAIN. Keep them enabled
   * for the whole stream; the previous temporary test masked starvation. */
  b=open_stream();send_mode=2;
  unsigned int read_next=0,ack_next=24;
  for(unsigned i=0;i<600;i++)
    {
      fake_now=10000+i*20;assert(!frame(b,i));
      if(b->has_sent && b->sent_seq>=read_next)
        {read_frames(read_next,b->sent_seq+1-read_next);read_next=b->sent_seq+1;}
      while(read_next>ack_next){ack(ack_next,false);ack_next+=25;}
    }
  for(unsigned i=0;b->count && i<10;i++)assert(!stream_flush(b));
  assert(!b->count);read_frames(read_next,600-read_next);
  assert(b->queue_high<10 && b->partial_writes>600*40 && !b->would_block);
  close_stream(b);
  b=open_stream();send_mode=3;
  for(unsigned i=0;i<5;i++){fake_now=10000+i*20;assert(!frame(b,i));}
  assert(b->count==5 && b->tx_offset==64);close_stream(b);
  b=open_stream();send_mode=1;
  for(unsigned i=0;i<100;i++){fake_now=10000+i*20;assert(!frame(b,i));}
  assert(b->would_block>0 && b->queue_high==100 && !b->partial_writes);
  fake_now=12000;assert(frame(b,100)==-EOVERFLOW);close_stream(b);
  b=open_stream();assert(!frame(b,0));wake_backend_gap(b);assert(frame(b,1)==-EPIPE);close_stream(b);
  b=open_stream();assert(!frame(b,0));assert(frame(b,2)==-EPIPE);close_stream(b);
  b=open_stream();fake_now=13001;assert(frame(b,0)==-ETIMEDOUT);close_stream(b);
  puts("WAKE_STREAM_ADAPTER_PASS real TCP fragmentation/partial-write/EAGAIN/sequence/fresh-vs-old-ACK/bounded-queue/system-heap-with-SDK-exhausted/zeroized-release; no board claim");
  return 0;
}
