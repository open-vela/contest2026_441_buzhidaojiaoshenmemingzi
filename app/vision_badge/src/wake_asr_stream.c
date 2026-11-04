/* SPDX-License-Identifier: Apache-2.0 */
/* Existing NuttX CONNECT tunnel + POSIX nonblocking socket. No ASR algorithm,
 * HTTP parser or WebSocket framing here. Audio callbacks still only enqueue. */
#include <nuttx/config.h>
#include <vision_badge/wake_backend.h>
#include <vision_badge/agora_control.h>
#include <netutils/webclient.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STREAM_TX_FRAMES 100u
#define STREAM_FRAME_BYTES 644u
#define STREAM_BATCH_FRAMES 5u
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
struct stream_packet_s { uint8_t bytes[STREAM_FRAME_BYTES]; };
_Static_assert(sizeof(struct stream_packet_s)==STREAM_FRAME_BYTES,"contiguous wire frames");
struct wake_backend_s
{
  cJSON *local;
  struct webclient_conn_s *conn;
  bool valid,ready,matched,has_sent,has_ack;
  char stream[33],rx[1025];
  size_t used,tx_offset;
  unsigned int next_seq,sent_seq,ack_seq,head,count;
  unsigned int partial_writes,would_block,queue_high,send_calls;
  uint64_t base_ms,previous_ms,last_reply_ms;
  struct stream_packet_s tx[STREAM_TX_FRAMES];
};
static uint64_t stream_now_ms(void)
{
  struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
  return (uint64_t)ts.tv_sec*1000u+ts.tv_nsec/1000000u;
}
static bool stream_id(const cJSON *id)
{
  return cJSON_IsString(id) && strlen(id->valuestring)==32 &&
         strspn(id->valuestring,"0123456789abcdef")==32;
}
static int stream_reply(struct wake_backend_s *b)
{
  cJSON *root=cJSON_ParseWithLengthOpts(b->rx,b->used+1,NULL,true);
  if (!root) return -EPROTO;
  cJSON *id=cJSON_GetObjectItemCaseSensitive(root,"stream");
  int ret=0;
  if (!stream_id(id)) ret=-EPROTO;
  else if (!b->ready)
    {
      cJSON *protocol=cJSON_GetObjectItemCaseSensitive(root,"protocol");
      if (!cJSON_IsNumber(protocol) || protocol->valuedouble!=2) ret=-EPROTO;
      else {memcpy(b->stream,id->valuestring,33);b->ready=true;}
    }
  else
    {
      cJSON *frame=cJSON_GetObjectItemCaseSensitive(root,"last_frame");
      cJSON *matched=cJSON_GetObjectItemCaseSensitive(root,"matched");
      if (strcmp(id->valuestring,b->stream) || !cJSON_IsNumber(frame) ||
          frame->valuedouble<0 || frame->valuedouble>UINT32_MAX ||
          (double)(uint32_t)frame->valuedouble!=frame->valuedouble ||
          !b->has_sent || frame->valuedouble>b->sent_seq ||
          (b->has_ack && frame->valuedouble<=b->ack_seq) ||
          (!cJSON_IsTrue(matched) && !cJSON_IsFalse(matched))) ret=-EPROTO;
      else
        {
          b->ack_seq=(uint32_t)frame->valuedouble;b->has_ack=true;
          uint64_t decision=b->base_ms+(uint64_t)b->ack_seq*20u;
          uint64_t now=stream_now_ms();
          /* ACK is correlated to its audio, not to whichever frame happens
           * to be arriving now. Late positives cannot wake the device. The
           * host resets its decoder after each emitted positive, so later
           * fresh speech remains usable on this same connection. */
          if (decision>=now || now-decision<=1500)
            {if(cJSON_IsTrue(matched))b->matched=true;ret=1;}
        }
    }
  cJSON_Delete(root);
  if (ret>=0) b->last_reply_ms=stream_now_ms();
  return ret;
}
static int stream_receive(struct wake_backend_s *b,bool *inferred)
{
  char data[1024];
  for (unsigned int reads=0;reads<4;reads++)
    {
      ssize_t count=recv(b->conn->sockfd,data,sizeof(data),0);
      if (count<0)
        {if(errno==EAGAIN || errno==EWOULDBLOCK)return 0;if(errno==EINTR)continue;return -errno;}
      if (!count) return -ECONNRESET;
      for (ssize_t i=0;i<count;i++)
        {
          if (data[i]=='\n')
            {
              b->rx[b->used]=0;
              int ret=stream_reply(b);b->used=0;
              if(ret<0)return ret;
              if(ret>0)*inferred=true;
            }
          else
            {
              if (!data[i] || b->used>=sizeof(b->rx)-1) return -E2BIG;
              b->rx[b->used++]=data[i];
            }
        }
    }
  return 0;
}
static int stream_flush(struct wake_backend_s *b)
{
  /* Keep 20ms capture frames intact, but coalesce up to 100ms into a
   * normal multi-segment write. Tiny sends consume scarce TCP write
   * chains faster than delayed ACKs release them on the embedded stack.
   * A partially sent frame always progresses immediately. */
  if(b->count<STREAM_BATCH_FRAMES && b->count && !b->tx_offset &&
     stream_now_ms()<b->base_ms+(uint64_t)(b->next_seq-b->count)*20+100)
    return 0;
  /* A positive short send is progress, not backpressure. Continue while
   * the nonblocking socket accepts data, with a fixed CPU work budget. */
  for (unsigned int writes=0;b->count && writes<64;writes++)
    {
      struct stream_packet_s *packet=&b->tx[b->head];
      unsigned int frames=b->count;
      if(frames>STREAM_BATCH_FRAMES)frames=STREAM_BATCH_FRAMES;
      if(frames>STREAM_TX_FRAMES-b->head)frames=STREAM_TX_FRAMES-b->head;
      size_t offered=(size_t)frames*STREAM_FRAME_BYTES-b->tx_offset;
      b->send_calls++;
      ssize_t count=send(b->conn->sockfd,packet->bytes+b->tx_offset,
                         offered,MSG_NOSIGNAL);
      if (count<0)
        {if(errno==EAGAIN || errno==EWOULDBLOCK){b->would_block++;return 0;}if(errno==EINTR)continue;return -errno;}
      if (!count) return -ECONNRESET;
      if((size_t)count<offered)b->partial_writes++;
      b->tx_offset+=(size_t)count;
      while(b->tx_offset>=STREAM_FRAME_BYTES)
        {
          packet=&b->tx[b->head];
          b->sent_seq=((uint32_t)packet->bytes[0]<<24)|((uint32_t)packet->bytes[1]<<16)|
                      ((uint32_t)packet->bytes[2]<<8)|packet->bytes[3];
          b->has_sent=true;b->tx_offset-=STREAM_FRAME_BYTES;
          memset(packet,0,sizeof(*packet));b->head=(b->head+1)%STREAM_TX_FRAMES;b->count--;
        }
    }
  return 0;
}
int wake_backend_open(bool release,struct wake_backend_s **out)
{
  (void)release;
  if(!out)return -EINVAL;
  *out=NULL;
  /* Ordinary TCP staging data, never DMA-owned. Reuse the system-heap
   * policy of JPEG snapshots and RX caching rather than taking another
   * 66KiB from the private Wi-Fi/audio SDK pool before MIC admission. */
  struct wake_backend_s *b=malloc(sizeof(*b));
  if(!b)return -ENOMEM;
  memset(b,0,sizeof(*b));
  int ret=agora_control_load(&b->local);
  if(!ret)ret=agora_control_wake_stream(b->local,&b->conn);
  if(!ret && (!b->conn || b->conn->tls))ret=-EPROTO;
  if(!ret)
    {
      int flags=fcntl(b->conn->sockfd,F_GETFL,0);
      if(flags<0 || fcntl(b->conn->sockfd,F_SETFL,flags|O_NONBLOCK)<0)ret=-errno;
      int one=1;
      if(!ret && setsockopt(b->conn->sockfd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one))<0)ret=-errno;
    }
  uint64_t deadline=stream_now_ms()+2000;
  size_t magic=0;
  while(!ret && !b->ready && stream_now_ms()<deadline)
    {
      if(magic<4)
        {
          ssize_t count=send(b->conn->sockfd,"R1A2"+magic,4-magic,MSG_NOSIGNAL);
          if(count>0)magic+=(size_t)count;
          else if(!count)ret=-ECONNRESET;
          else if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)ret=-errno;
        }
      if(!ret && magic==4){bool inferred=false;ret=stream_receive(b,&inferred);}
      if(!ret && !b->ready)usleep(2000);
    }
  if(!ret && !b->ready)ret=-ETIMEDOUT;
  if(ret<0){wake_backend_close(b);return ret;}
  b->valid=true;*out=b;return 0;
}
void wake_backend_gap(struct wake_backend_s *b)
{
  if(b)b->valid=false;
}
int wake_backend_feed(struct wake_backend_s *b,const int16_t pcm[320],
                      uint64_t end_ms,float *score,bool *inferred)
{
  if(!b || !pcm || !score || !inferred)return -EINVAL;
  *score=0;*inferred=false;
  if(!b->valid)return -EPIPE;
  if(b->previous_ms && end_ms-b->previous_ms!=20)return -EPIPE;
  b->previous_ms=end_ms;
  if(!b->base_ms)b->base_ms=end_ms;
  int ret=stream_flush(b);
  if(ret<0)return ret;
  if(b->count==STREAM_TX_FRAMES || b->next_seq==UINT32_MAX)
    {
      syslog(LOG_ERR,"R1 WAKE stream overflow queued=%u partial=%u wouldblock=%u sent=%u ack=%u\n",
             b->count,b->partial_writes,b->would_block,b->sent_seq,b->ack_seq);
      return -EOVERFLOW;
    }
  struct stream_packet_s *packet=&b->tx[(b->head+b->count)%STREAM_TX_FRAMES];
  uint32_t seq=b->next_seq++;
  packet->bytes[0]=seq>>24;packet->bytes[1]=seq>>16;packet->bytes[2]=seq>>8;packet->bytes[3]=seq;
  memcpy(packet->bytes+4,pcm,640);b->count++;
  if(b->count>b->queue_high)b->queue_high=b->count;
  ret=stream_flush(b);
  if(!ret)ret=stream_receive(b,inferred);
  if(ret<0)return ret;
  if(stream_now_ms()-b->last_reply_ms>3000)return -ETIMEDOUT;
  if(b->matched){b->matched=false;*inferred=true;return 1;}
  return 0;
}
void wake_backend_close(struct wake_backend_s *b)
{
  if(!b)return;
  syslog(LOG_INFO,"R1 WAKE stream closed frames=%u sent=%u ack=%u queue_high=%u partial=%u wouldblock=%u calls=%u batch=5\n",
         b->next_seq,b->sent_seq,b->ack_seq,b->queue_high,b->partial_writes,b->would_block,b->send_calls);
  if(b->conn){webclient_conn_close(b->conn);webclient_conn_free(b->conn);}
  cJSON_Delete(b->local);
  memset(b,0,sizeof(*b));free(b);
}
