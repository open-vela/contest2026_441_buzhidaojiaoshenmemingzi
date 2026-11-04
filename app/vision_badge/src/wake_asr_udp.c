/* SPDX-License-Identifier: Apache-2.0 */
/* POSIX datagrams + existing mbedTLS HMAC, Audio owner and host recognizer.
 * No TCP retry backlog and no new codec / DSP / speech algorithm. */
#include <nuttx/config.h>
#include <vision_badge/wake_backend.h>
#include <vision_badge/agora_control.h>
#include <mbedtls/md.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UDP_PACKET_BYTES 1320u
#define UDP_ACK_BYTES 41u
#define UDP_CAPTURE_STAMPS 128u /* 2.56 s; exceeds the 1.5-s ACK age bound. */
struct wake_backend_s
{
  cJSON *local;
  mbedtls_md_context_t hmac;
  int fd;
  bool valid,has_sent,has_ack;
  char stream[33];
  uint8_t nonce[16],packet[UDP_PACKET_BYTES];
  uint32_t sequence,sent_seq,ack_seq,dropped,received,ignored;
  uint64_t previous_ms,last_reply_ms;
  uint64_t sample_grid_base_ms;
  uint32_t sample_ahead_max_ms,stale_acks;
  uint64_t last_datagram_ms;
  uint32_t reply_gap_max_ms;
  struct
  {
    uint64_t captured_ms;
    uint32_t sequence;
    bool sent;
  } stamps[UDP_CAPTURE_STAMPS];
};

static uint64_t udp_now_ms(void)
{
  struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
  return (uint64_t)ts.tv_sec*1000u+ts.tv_nsec/1000000u;
}
static int udp_failed(struct wake_backend_s *b,const char *stage,int error)
{
  uint64_t now=udp_now_ms();
  syslog(LOG_ERR,"R1 WAKE udp failure stage=%s error=%d reply_age_ms=%llu datagram_age_ms=%llu frames=%u sent=%u ack=%u\n",
         stage,error,(unsigned long long)(now-b->last_reply_ms),
         (unsigned long long)(b->last_datagram_ms?now-b->last_datagram_ms:now-b->last_reply_ms),
         b->sequence,b->sent_seq,b->ack_seq);
  return error;
}
static int udp_tag(struct wake_backend_s *b,const uint8_t *data,size_t bytes,
                    uint8_t output[32])
{
  if(mbedtls_md_hmac_reset(&b->hmac) ||
     mbedtls_md_hmac_update(&b->hmac,data,bytes) ||
     mbedtls_md_hmac_finish(&b->hmac,output))return -EIO;
  return 0;
}
static bool udp_id(const cJSON *id)
{
  return cJSON_IsString(id) && strlen(id->valuestring)==32 &&
         strspn(id->valuestring,"0123456789abcdef")==32;
}
static uint32_t udp_u32(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}

int wake_backend_open(bool release,struct wake_backend_s **out)
{
  (void)release;
  if(!out)return -EINVAL;
  *out=NULL;
  struct wake_backend_s *b=calloc(1,sizeof(*b));
  if(!b)return -ENOMEM;
  b->fd=-1;mbedtls_md_init(&b->hmac);
  int ret=agora_control_load(&b->local);
  struct sockaddr_in peer;
  unsigned char key[32]={0};
  if(!ret)ret=agora_control_wake_udp_target(b->local,&peer,key);
  if(!ret)
    {
      const mbedtls_md_info_t *info=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
      if(!info || mbedtls_md_setup(&b->hmac,info,1) ||
         mbedtls_md_hmac_starts(&b->hmac,key,sizeof(key)))ret=-EIO;
    }
  explicit_bzero(key,sizeof(key));
  if(!ret)
    {
      b->fd=socket(AF_INET,SOCK_DGRAM,0);
      if(b->fd<0)ret=-errno;
    }
  if(!ret && connect(b->fd,(struct sockaddr*)&peer,sizeof(peer))<0)ret=-errno;
  if(!ret)
    {
      int flags=fcntl(b->fd,F_GETFL,0);
      if(flags<0 || fcntl(b->fd,F_SETFL,flags|O_NONBLOCK)<0)ret=-errno;
    }
  cJSON *reply=NULL;
  if(!ret)ret=agora_control_wake(b->local,"udp-start","{}",2,NULL,&reply);
  if(!ret)
    {
      cJSON *id=cJSON_GetObjectItemCaseSensitive(reply,"stream");
      cJSON *protocol=cJSON_GetObjectItemCaseSensitive(reply,"protocol");
      cJSON *port=cJSON_GetObjectItemCaseSensitive(reply,"udp_port");
      if(udp_id(id))memcpy(b->stream,id->valuestring,33);
      if(!b->stream[0] || !cJSON_IsNumber(protocol) || protocol->valuedouble!=3 ||
         !cJSON_IsNumber(port) || port->valuedouble!=ntohs(peer.sin_port))ret=-EPROTO;
      else
        for(unsigned int i=0;i<16;i++)
          {
            unsigned int byte=0;
            for(unsigned int j=0;j<2;j++)
              {char c=b->stream[i*2+j];byte=byte*16+(c<='9'?c-'0':c-'a'+10);}
            b->nonce[i]=(uint8_t)byte;
          }
    }
  cJSON_Delete(reply);
  if(ret<0){wake_backend_close(b);return ret;}
  b->valid=true;b->last_reply_ms=udp_now_ms();*out=b;return 0;
}

void wake_backend_gap(struct wake_backend_s *b)
{if(b)b->valid=false;}

static int udp_receive(struct wake_backend_s *b,bool *inferred)
{
  int matched=0;
  for(unsigned int reads=0;reads<8;reads++)
    {
      uint8_t raw[UDP_ACK_BYTES+1],tag[32];
      ssize_t count=recv(b->fd,raw,sizeof(raw),0);
      if(count<0)
        {
          if(errno==EAGAIN || errno==EWOULDBLOCK)break;
          if(errno==EINTR)continue;
          return udp_failed(b,"recv",-errno);
        }
      b->last_datagram_ms=udp_now_ms();
      if(count!=UDP_ACK_BYTES || memcmp(raw,"R1K1",4) ||
         memcmp(raw+4,b->nonce,16) || raw[24]>1)
        {b->ignored++;continue;}
      int ret=udp_tag(b,raw,25,tag);
      if(ret<0)return ret;
      unsigned int different=0;
      for(unsigned int i=0;i<16;i++)different|=tag[i]^raw[25+i];
      if(different){b->ignored++;continue;}
      uint32_t frame=udp_u32(raw+20);
      if(!b->has_sent || frame>b->sent_seq || (b->has_ack && frame<=b->ack_seq))
        {b->ignored++;continue;}
      unsigned int slot=frame%UDP_CAPTURE_STAMPS;
      if(!b->stamps[slot].sent || b->stamps[slot].sequence!=frame)
        {b->ignored++;continue;} /* Dropped or already overwritten audio. */
      b->has_ack=true;b->ack_seq=frame;b->received++;
      uint64_t now=udp_now_ms(),decision=b->stamps[slot].captured_ms;
      /* Numeric diagnostic only: quantify how far the former sample-count
       * dating would run ahead. It must never control ACK acceptance. */
      uint64_t grid=b->sample_grid_base_ms+(uint64_t)frame*20u;
      if(grid>now)
        {
          uint64_t ahead=grid-now;
          if(ahead>b->sample_ahead_max_ms)
            b->sample_ahead_max_ms=ahead>UINT32_MAX?UINT32_MAX:(uint32_t)ahead;
        }
      /* Only a timely ACK of actually sent audio can trigger or renew the
       * stream watchdog. A valid but delayed datagram cannot wake later. */
      if(decision>now || now-decision>1500){b->ignored++;b->stale_acks++;continue;}
      uint64_t gap=now-b->last_reply_ms;
      if(gap>b->reply_gap_max_ms) b->reply_gap_max_ms=gap>UINT32_MAX?UINT32_MAX:(uint32_t)gap;
      b->last_reply_ms=now;*inferred=true;
      if(raw[24])matched=1;
    }
  return matched;
}

int wake_backend_feed(struct wake_backend_s *b,const int16_t pcm[320],
                      uint64_t end_ms,float *score,bool *inferred)
{
  if(!b || !pcm || !score || !inferred)return -EINVAL;
  *score=0;*inferred=false;
  if(!b->valid)return udp_failed(b,"capture-gap",-EPIPE);
  /* UDP has its own frame sequence. Capture callbacks can complete multiple
   * frames at one observed timestamp; neither require a perfect 20-ms clock
   * nor reconstruct capture time from samples over an unbounded stream.
   */
  if(b->sequence && end_ms<b->previous_ms)return -EPIPE;
  if(end_ms>udp_now_ms())return -EINVAL;
  if(b->sequence==UINT32_MAX)return -EOVERFLOW;
  b->previous_ms=end_ms;
  uint32_t seq=b->sequence++;
  if(!seq)b->sample_grid_base_ms=end_ms+20u;
  unsigned int slot=seq%UDP_CAPTURE_STAMPS;
  b->stamps[slot].captured_ms=end_ms;
  b->stamps[slot].sequence=seq;
  b->stamps[slot].sent=false;
  uint8_t *raw=b->packet;
  memcpy(raw,"R1U1",4);memcpy(raw+4,b->nonce,16);
  raw[20]=seq>>24;raw[21]=seq>>16;raw[22]=seq>>8;raw[23]=seq;
  /* Previous current frame stays available even if its send was dropped. */
  memcpy(raw+24,raw+664,640);memcpy(raw+664,pcm,640);
  uint8_t tag[32];int ret=udp_tag(b,raw,1304,tag);
  if(ret<0)return ret;
  memcpy(raw+1304,tag,16);
  ssize_t sent=send(b->fd,raw,sizeof(b->packet),0);
  if(sent<0)
    {
      if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=ENOBUFS && errno!=EINTR)return udp_failed(b,"send",-errno);
      b->dropped++;
    }
  else if(sent!=(ssize_t)sizeof(b->packet))return -EIO;
  else
    {
      b->has_sent=true;b->sent_seq=seq;
      b->stamps[slot].sent=true;
      /* Each successful datagram also carries the preceding frame. A frame
       * whose first send failed is genuinely sent by this redundancy. */
      if(seq)
        {
          unsigned int previous=(seq-1u)%UDP_CAPTURE_STAMPS;
          if(b->stamps[previous].sequence==seq-1u)
            b->stamps[previous].sent=true;
        }
    }
  ret=udp_receive(b,inferred);
  if(ret<0)return ret;
  if(udp_now_ms()-b->last_reply_ms>3000)return udp_failed(b,"ack-watchdog",-ETIMEDOUT);
  return ret;
}

void wake_backend_close(struct wake_backend_s *b)
{
  if(!b)return;
  syslog(LOG_INFO,"R1 WAKE udp closed frames=%u sent=%u ack=%u dropped=%u replies=%u ignored=%u stale_acks=%u sample_clock_ahead_max_ms=%u reply_gap_max_ms=%u\n",
         b->sequence,b->sent_seq,b->ack_seq,b->dropped,b->received,b->ignored,
         b->stale_acks,b->sample_ahead_max_ms,b->reply_gap_max_ms);
  if(b->fd>=0)close(b->fd);
  if(b->stream[0])
    {
      char body[64];cJSON *reply=NULL;
      snprintf(body,sizeof(body),"{\"stream\":\"%s\"}",b->stream);
      (void)agora_control_wake(b->local,"stop",body,strlen(body),NULL,&reply);
      cJSON_Delete(reply);
    }
  cJSON_Delete(b->local);mbedtls_md_free(&b->hmac);
  explicit_bzero(b,sizeof(*b));free(b);
}
