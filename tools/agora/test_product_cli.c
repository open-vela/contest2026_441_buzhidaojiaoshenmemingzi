/* SPDX-License-Identifier: Apache-2.0 */
/* Real CP command-line parser; RPC boundary mocked, no board/cloud/audio. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <vision_badge/rpc.h>
int vision_badge_rpc_initialize(void) { return 0; }
bool vision_badge_rpc_ready(void) { return true; }
static int capture(const char *kind,const char *action,struct vision_badge_rpc_result_s *result)
{
  memset(result,0,sizeof(*result));
  printf("RPC_CAPTURE %s %s\n",kind,action);
  snprintf(result->text,sizeof(result->text),"fixture");return 0;
}
int vision_badge_rpc_audio_diagnostic(const char *s,struct vision_badge_rpc_result_s *r) { return capture("audio",s,r); }
int vision_badge_rpc_power(const char *s,struct vision_badge_rpc_result_s *r) { return capture("power",s,r); }
int vision_badge_rpc_camera(const char *s,struct vision_badge_rpc_result_s *r) { return capture("camera",s,r); }
int vision_badge_rpc_agora(const char *s,struct vision_badge_rpc_result_s *r) { return capture("agora",s,r); }
int vision_badge_rpc_conversation(const char *s,struct vision_badge_rpc_result_s *r) { return capture("voice",s,r); }
int vision_badge_rpc_battery(struct vision_badge_rpc_result_s *r) { return capture("battery","status",r); }
int vision_badge_rpc_codec_selftest(struct vision_badge_rpc_result_s *r) { return capture("codec","test",r); }
int vision_badge_rpc_audio_loopback(unsigned int seconds,unsigned int volume,struct vision_badge_rpc_result_s *r)
{ (void)seconds;(void)volume;(void)r;return -ENOSYS; }
int vision_badge_rpc_query(const char *s,unsigned int timeout,struct vision_badge_rpc_result_s *r)
{ (void)s;(void)timeout;(void)r;return -ENOSYS; }
int vision_badge_rpc_provision(const char *ssid,const char *password,const char *key,unsigned int timeout,struct vision_badge_rpc_result_s *r)
{ (void)ssid;(void)password;(void)key;(void)timeout;(void)r;return -ENOSYS; }
#include "../../app/vision_badge/src/vision_badge_client_main.c"
