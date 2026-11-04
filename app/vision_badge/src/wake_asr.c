/* SPDX-License-Identifier: Apache-2.0 */
/* Transport adapter, not an ASR implementation. sherpa-onnx runs on the LAN
 * host. The existing wake worker alone owns Audio/camera handoff. */
#include <nuttx/config.h>
#include <vision_badge/wake_backend.h>
#include <vision_badge/agora_control.h>
#include <arch/chip/bk7258_psram.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct wake_backend_s
{
  cJSON *local;
  char stream[33];
  unsigned int sequence, count;
  uint64_t previous_ms;
  bool valid;
  int16_t pcm[8000];
};

static bool stream_valid(const cJSON *value)
{
  if (!cJSON_IsString(value) || strlen(value->valuestring)!=32) return false;
  return strspn(value->valuestring,"0123456789abcdef")==32;
}

int wake_backend_open(bool release,struct wake_backend_s **out)
{
  /* Every capture resumes with a newly minted, empty ASR stream; no previous
   * transcription can trigger again after voice/RPC/power ownership changes. */
  (void)release;
  if (!out) return -EINVAL;
  *out=NULL;
  struct wake_backend_s *backend=bk7258_psram_malloc(sizeof(*backend));
  if (!backend) return -ENOMEM;
  memset(backend,0,sizeof(*backend));
  int ret=agora_control_load(&backend->local);
  cJSON *reply=NULL;
  if (!ret) ret=agora_control_wake(backend->local,"start","{}",2,NULL,&reply);
  if (!ret)
    {
      cJSON *stream=cJSON_GetObjectItemCaseSensitive(reply,"stream");
      cJSON *protocol=cJSON_GetObjectItemCaseSensitive(reply,"protocol");
      if (!stream_valid(stream) || !cJSON_IsNumber(protocol) || protocol->valuedouble!=1)
        ret=-EPROTO;
      else { memcpy(backend->stream,stream->valuestring,33); backend->valid=true; }
    }
  cJSON_Delete(reply);
  if (ret<0) { wake_backend_close(backend); return ret; }
  *out=backend; return 0;
}

void wake_backend_gap(struct wake_backend_s *backend)
{
  /* Don't splice speech across a queue drop or network stall. Stop explicitly;
   * normal wake-start can establish a fresh stream after the error is seen. */
  if (backend) { backend->valid=false; memset(backend->pcm,0,sizeof(backend->pcm)); }
}

int wake_backend_feed(struct wake_backend_s *backend,const int16_t pcm[320],
                      uint64_t end_ms,float *score,bool *inferred)
{
  if (!backend || !pcm || !score || !inferred) return -EINVAL;
  *score=0; *inferred=false; /* Text match has no acoustic probability score. */
  if (!backend->valid) return -EPIPE;
  if (backend->previous_ms && (end_ms<=backend->previous_ms || end_ms-backend->previous_ms!=20))
    { wake_backend_gap(backend); return -EPIPE; }
  backend->previous_ms=end_ms;
  memcpy(backend->pcm+backend->count,pcm,640); backend->count+=320;
  if (backend->count<8000) return 0;
  *inferred=true;
  char sequence[80];
  snprintf(sequence,sizeof(sequence),"X-R1-Wake: %s:%u",backend->stream,backend->sequence);
  cJSON *reply=NULL;
  int ret=agora_control_wake(backend->local,"frame",backend->pcm,sizeof(backend->pcm),sequence,&reply);
  memset(backend->pcm,0,sizeof(backend->pcm)); backend->count=0;
  if (!ret)
    {
      cJSON *stream=cJSON_GetObjectItemCaseSensitive(reply,"stream");
      cJSON *seq=cJSON_GetObjectItemCaseSensitive(reply,"seq");
      cJSON *matched=cJSON_GetObjectItemCaseSensitive(reply,"matched");
      if (!stream_valid(stream) || strcmp(stream->valuestring,backend->stream) ||
          !cJSON_IsNumber(seq) || seq->valuedouble!=backend->sequence ||
          (!cJSON_IsTrue(matched) && !cJSON_IsFalse(matched))) ret=-EPROTO;
      else { ret=cJSON_IsTrue(matched)?1:0; backend->sequence++; }
    }
  cJSON_Delete(reply);
  if (ret<0) backend->valid=false;
  return ret;
}

void wake_backend_close(struct wake_backend_s *backend)
{
  if (!backend) return;
  if (backend->stream[0])
    {
      char body[64];cJSON *reply=NULL;
      snprintf(body,sizeof(body),"{\"stream\":\"%s\"}",backend->stream);
      (void)agora_control_wake(backend->local,"stop",body,strlen(body),NULL,&reply);
      cJSON_Delete(reply);
    }
  cJSON_Delete(backend->local);
  memset(backend,0,sizeof(*backend)); bk7258_psram_free(backend);
}
