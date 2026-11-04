/* SPDX-License-Identifier: Apache-2.0 */
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/statfs.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <syslog.h>
static int save_fail;
static unsigned int save_diagnostics;
static char last_save_diagnostic[128];
static size_t test_fwrite(const void *data, size_t size, size_t count, FILE *file)
{ if (save_fail == 1) { errno=EIO;return 0; } return fwrite(data,size,count,file); }
static int test_fflush(FILE *file)
{ if (save_fail == 2) { errno=EIO;return -1; } return fflush(file); }
static int test_fsync(int fd)
{ if (save_fail == 3) { errno=EIO;return -1; } return fsync(fd); }
static int test_fclose(FILE *file)
{ int ret=fclose(file);if(save_fail==4){errno=EIO;return -1;}return ret; }
static int test_rename(const char *old, const char *next)
{ if(save_fail==5){errno=EIO;return -1;}return rename(old,next); }
static void test_syslog(int priority, const char *format, ...)
{
  (void)priority;
  if (!strncmp(format,"agora: config-save ",19))
    {
      va_list args;va_start(args,format);
      vsnprintf(last_save_diagnostic,sizeof(last_save_diagnostic),format,args);
      va_end(args);save_diagnostics++;
    }
}
static int storage_mode;
static unsigned int mount_calls;
static bool storage_mounted;
static int test_statfs(const char *path, struct statfs *fs)
{
  (void)path;
  memset(fs, 0, sizeof(*fs));
  if (storage_mode == 6 || (storage_mode == 8 && mount_calls) ||
      (storage_mode == 9 && storage_mounted))
    { fs->f_type = 0x4d44; return 0; }
  if (storage_mode == 10) { fs->f_type = 0x1234; return 0; }
  errno = ENOENT; return -1;
}
static int test_stat(const char *path, struct stat *info)
{
  if (!strncmp(path, "/dev/mmcsd", 10))
    {
      if (storage_mode == 4 ||
          (storage_mode == 3 && !strcmp(path, "/dev/mmcsd0p0")))
        { errno = ENOENT; return -1; }
      memset(info, 0, sizeof(*info)); return 0;
    }
  return stat(path, info);
}
static int test_mkdir(const char *path, mode_t mode)
{
  if (!strcmp(path, "/mnt") || !strcmp(path, "/mnt/sdnand"))
    { errno = storage_mode == 5 ? EACCES : EEXIST; return -1; }
  return mkdir(path, mode);
}
static int test_mount(const char *device, const char *target,
                      const char *type, unsigned long flags, const void *data)
{
  (void)flags; (void)data; mount_calls++;
  if (strcmp(target, "/mnt/sdnand") || strcmp(type, "vfat")) return -1;
  if (strcmp(device, storage_mode == 3 ? "/dev/mmcsd0" : "/dev/mmcsd0p0")) return -1;
  if (storage_mode == 1 || storage_mode == 2 || storage_mode == 7 || storage_mode == 8 || storage_mode == 10)
    { errno = storage_mode == 1 ? EIO : storage_mode == 2 ? EBUSY : ENOTDIR; return -1; }
  if (storage_mode == 9) storage_mounted = true;
  return 0;
}
#define __NuttX__ 1
#define CONFIG_BK7258_AP_CORE 1
#define CONFIG_BK7258_AIDK_CAMERA 1
#define CONFIG_CONTEST2026_441_VISION_BADGE_MIMO_STORAGE_DEVICE "/dev/mmcsd0"
#define stat(path, info) test_stat(path, info)
#define statfs(path, info) test_statfs(path, info)
#define MSDOS_SUPER_MAGIC 0x4d44
#define mkdir(path, mode) test_mkdir(path, mode)
#define mount(device, target, type, flags, data) test_mount(device, target, type, flags, data)
#define fwrite test_fwrite
#define fflush test_fflush
#define fsync test_fsync
#define fclose test_fclose
#define rename test_rename
#define syslog test_syslog
#include "../../app/vision_badge/src/agora_control.c"
#undef stat
#undef statfs
#undef mkdir
#undef mount
#define AGORA_CONFIG AGORA_CONFIG_DIR "/agora.cfg"
/* Generated from the real service loader, not a rewritten test substitute. */
#include "agora-load-test.inc"
#include <assert.h>
static unsigned int calls;
static unsigned int mode;
static unsigned int tunnel_detached,tunnel_closed,tunnel_freed;
void webclient_get_tunnel(struct webclient_context *ctx,struct webclient_conn_s **conn)
{assert(ctx->http_status/100==2);*conn=calloc(1,sizeof(**conn));assert(*conn);tunnel_detached++;}
void webclient_conn_close(struct webclient_conn_s *conn){assert(conn);tunnel_closed++;}
void webclient_conn_free(struct webclient_conn_s *conn){assert(conn);tunnel_freed++;free(conn);}
void webclient_set_defaults(struct webclient_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }
void webclient_set_static_body(struct webclient_context *ctx, const void *body, size_t size)
{ ctx->test_body = body; ctx->test_bodylen = size; }
int webclient_perform(struct webclient_context *ctx)
{
  calls++;
  if(!strcmp(ctx->method,"CONNECT"))
    {
      assert(ctx->flags==WEBCLIENT_FLAG_TUNNEL && ctx->timeout_sec==2);
      assert(!strcmp(ctx->proxy,"http://192.168.110.25:8766") && !ctx->url);
      assert(!strcmp(ctx->tunnel_target_host,"192.168.110.25") && ctx->tunnel_target_port==8766);
      assert(ctx->proxy_nheaders==2 && strstr(ctx->proxy_headers[0],"Authorization: Bearer ")==ctx->proxy_headers[0]);
      assert(!strcmp(ctx->proxy_headers[1],"X-R1-Wake-Stream: 1"));
      if(mode==1)return -ETIMEDOUT;
      if(mode==3)return ctx->header_callback("Location: http://example.invalid/",false,NULL);
      ctx->http_status=mode==2?401:mode==6?204:200;return 0;
    }
  bool wake = strstr(ctx->url,"/wake/")!=NULL;
  if(wake)
    {
      assert(ctx->timeout_sec==2);
      if(strstr(ctx->url,"/wake/frame"))
        {assert(ctx->nheaders==3 && !strcmp(ctx->headers[2],"X-R1-Wake: 0123456789abcdef0123456789abcdef:0"));}
      else assert(ctx->nheaders==2);
    }
  else if (ctx->nheaders == 3)
    {
      assert(!strcmp(ctx->headers[2], "X-R1-Vision-Request: 0123456789abcdef0123456789abcdef"));
      assert(ctx->timeout_sec >= 1 && ctx->timeout_sec <= 10);
    }
  else assert(ctx->nheaders == 2 && ctx->timeout_sec == 30);
  bool scene = strstr(ctx->url, "/vision/frame") != NULL ||
               strstr(ctx->url, "/vision/capture") != NULL;
  bool capture_failed = scene && !strcmp(ctx->headers[0], "Content-Type: application/json");
  assert(!strncmp(ctx->url, "http://192.168.110.25:8766/", 25));
  assert(!strcmp(ctx->method, "POST"));
  if(wake && strstr(ctx->url,"/wake/frame"))
    {assert(ctx->test_bodylen==16000 && !strcmp(ctx->headers[0],"Content-Type: application/octet-stream"));}
  else if (capture_failed)
    {
      assert(ctx->timeout_sec <= 2);
      assert(ctx->test_bodylen == strlen("{\"capture_failed\":true}"));
      assert(!memcmp(ctx->test_body, "{\"capture_failed\":true}", ctx->test_bodylen));
    }
  else if (scene)
    { assert(ctx->test_bodylen == 6); assert(!strcmp(ctx->headers[0], "Content-Type: image/jpeg")); }
  else assert(ctx->test_bodylen == strlen(ctx->test_body));
  if (mode == 1) return -ETIMEDOUT;
  if (mode == 2) { ctx->http_status = 401; return 0; }
  if (mode == 36) { ctx->http_status = 429; return 0; }
  if (mode == 3) return ctx->header_callback("Location: http://example.invalid/", false, NULL);
  ctx->http_status = 200;
  char response[4200];
  if (mode == 4) { memset(response, 'x', sizeof(response)); }
  else if (mode == 5) snprintf(response, sizeof(response), "not-json");
  else if (wake) snprintf(response,sizeof(response),"{\"ok\":true}");
  else if (capture_failed)
    snprintf(response, sizeof(response), mode == 7 ? "{\"ok\":false}" : "{\"ok\":true}");
  else if (scene)
    snprintf(response, sizeof(response), mode == 6 ? "{\"text\":\"bad\",\"navigation\":true}" : "{\"text\":\"scene-reply\",\"navigation\":false}");
  else if (strstr(ctx->url, "/start"))
    {
      assert(!strcmp(ctx->test_body, "{\"vision_tool_v1\":true,\"vision_poll_v2\":true}"));
      snprintf(response, sizeof(response), "{\"lease\":\"%064d\"}", 0);
    }
  else if (mode >= 20 && mode <= 35)
    {
      snprintf(response, sizeof(response),
        "{\"ok\":true,%s\"vision_request\":{\"request_id\":\"%s\",\"action\":\"%s\",\"remaining_ms\":%s,\"question\":%s}}",
        mode < 28 ? "" : mode == 28 ? "\"server_elapsed_ms\":60," :
        mode == 29 ? "\"server_elapsed_ms\":0," : mode == 30 ? "\"server_elapsed_ms\":\"0\"," :
        mode == 31 ? "\"server_elapsed_ms\":-1," : mode == 32 ? "\"server_elapsed_ms\":1.5," :
        mode == 33 ? "\"server_elapsed_ms\":30001," : mode == 34 ? "\"server_elapsed_ms\":true," :
        "\"server_elapsed_ms\":null,",
        mode == 21 ? "bad" : "0123456789abcdef0123456789abcdef",
        mode == 22 ? "other" : "capture_scene",
        mode == 23 ? "0" : mode == 24 ? "1.5" : mode == 25 ? "10001" : mode >= 26 ? "25" : "10000",
        "\"look ahead\"");
      if (mode == 26 || mode == 28 || mode == 29) usleep(60000);
    }
  else snprintf(response, sizeof(response), "{\"ok\":true}");
  int size = sizeof(response);
  char *ptr = response;
  if (mode == 27)
    {
      assert(ctx->sink_callback(&ptr, 0, 1, &size, ctx->sink_callback_arg) == 0);
      usleep(60000); /* Slow response body must still expire. */
      return ctx->sink_callback(&ptr, 1, (int)strlen(response), &size, ctx->sink_callback_arg);
    }
  return ctx->sink_callback(&ptr, 0, mode == 4 ? (int)sizeof(response) : (int)strlen(response),
                            &size, ctx->sink_callback_arg);
}
int main(void)
{
  const char *pools[] = {
    "             total bsize nused nfree nifree nwaiter maxalloc\n"
    "g_tcp_connections: 2048 256 2 6 0 0 0\n"
    " g_cbprealloc: 1152 36 19 13 0 0 0\n",
    " g_cbprealloc: 576 36 16 0 0 0 0\n",
    " g_cbprealloc: 576 0 0 16 0 0 0\n",
    " g_cbprealloc: 576 36 0 9999 0 0 0\n",
    " g_other_pool: 576 36 0 16 0 0 0\n",
    /* A longer preceding pool can lose its newline in upstream procfs. */
    "g_tcp_connections:       2048      256        2        6        0        0     0"
    "g_cbprealloc:       1152       36       19       13        0        0        0\n"
  };
  const int expected_free[] = {13, 0, -1, -1, -1, 13};
  for (size_t i = 0; i < sizeof(pools) / sizeof(pools[0]); i++)
    {
      FILE *stream = tmpfile(); assert(stream);
      assert(fputs(pools[i], stream) >= 0); rewind(stream);
      assert(callback_free(stream) == expected_free[i]); fclose(stream);
    }
  cJSON *local = cJSON_Parse("{\"server_host\":\"192.168.110.25\",\"server_port\":8766,\"device_key\":\"0000000000000000000000000000000000000000000000000000000000000000\"}");
  assert(local);
  struct sockaddr_in udp_peer;unsigned char udp_key[32];
  assert(!agora_control_wake_udp_target(local,&udp_peer,udp_key));
  assert(udp_peer.sin_family==AF_INET && ntohs(udp_peer.sin_port)==8766);
  for(unsigned int i=0;i<32;i++)assert(udp_key[i]==0);
  cJSON_ReplaceItemInObject(local,"device_key",cJSON_CreateString("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
  assert(!agora_control_wake_udp_target(local,&udp_peer,udp_key));
  const unsigned char expected_key[]={1,35,69,103,137,171,205,239};
  for(unsigned int i=0;i<32;i++)assert(udp_key[i]==expected_key[i%8]);
  cJSON_ReplaceItemInObject(local,"device_key",cJSON_CreateString("g123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
  assert(agora_control_wake_udp_target(local,&udp_peer,udp_key)==-EINVAL);
  cJSON_ReplaceItemInObject(local,"device_key",cJSON_CreateString("0000000000000000000000000000000000000000000000000000000000000000"));
  cJSON_ReplaceItemInObject(local,"server_host",cJSON_CreateString("8.8.8.8"));
  assert(agora_control_wake_udp_target(local,&udp_peer,udp_key)==-EACCES);
  cJSON_ReplaceItemInObject(local,"server_host",cJSON_CreateString("192.168.110.25"));
  struct webclient_conn_s *tunnel=NULL;
  assert(!agora_control_wake_stream(local,&tunnel) && tunnel && tunnel_detached==1);
  webclient_conn_close(tunnel);webclient_conn_free(tunnel);tunnel=NULL;
  mode=1;assert(agora_control_wake_stream(local,&tunnel)==-ETIMEDOUT && !tunnel);
  mode=2;assert(agora_control_wake_stream(local,&tunnel)==-EACCES && !tunnel);
  mode=3;assert(agora_control_wake_stream(local,&tunnel)==-EACCES && !tunnel);
  mode=6;assert(agora_control_wake_stream(local,&tunnel)==-EPROTO && !tunnel);
  assert(tunnel_detached==2 && tunnel_closed==2 && tunnel_freed==2);
  cJSON_ReplaceItemInObject(local,"server_host",cJSON_CreateString("8.8.8.8"));
  unsigned int before_tunnel=calls;
  assert(agora_control_wake_stream(local,&tunnel)==-EACCES && calls==before_tunnel);
  cJSON_ReplaceItemInObject(local,"server_host",cJSON_CreateString("192.168.110.25"));
  mode=0;puts("WAKE_STREAM_NATIVE_TUNNEL_PASS private endpoint/auth/no-redirect/unexpected-2xx-cleanup");
  const char *sequence="X-R1-Wake: 0123456789abcdef0123456789abcdef:0";
  uint8_t pcm[16000]={0};cJSON *wake_reply=NULL;
  assert(agora_control_wake(local,"start","{}",2,NULL,&wake_reply)==0);cJSON_Delete(wake_reply);wake_reply=NULL;
  assert(agora_control_wake(local,"udp-start","{}",2,NULL,&wake_reply)==0);cJSON_Delete(wake_reply);wake_reply=NULL;
  mode=36;
  assert(agora_control_wake(local,"udp-start","{}",2,NULL,&wake_reply)==-EAGAIN && !wake_reply);
  assert(agora_control_wake(local,"start","{}",2,NULL,&wake_reply)==-EREMOTEIO && !wake_reply);
  cJSON *limited_session=NULL;char limited_lease[65];
  assert(agora_control_start(local,&limited_session,limited_lease)==-EREMOTEIO && !limited_session);
  mode=0;
  assert(agora_control_wake(local,"frame",pcm,sizeof(pcm),sequence,&wake_reply)==0);cJSON_Delete(wake_reply);wake_reply=NULL;
  unsigned int before_wake=calls;
  assert(agora_control_wake(local,"frame",pcm,640,sequence,&wake_reply)==-EINVAL && calls==before_wake);
  cJSON_ReplaceItemInObject(local,"server_host",cJSON_CreateString("8.8.8.8"));
  assert(agora_control_wake(local,"start","{}",2,NULL,&wake_reply)==-EACCES && calls==before_wake);
  cJSON_ReplaceItemInObject(local,"server_host",cJSON_CreateString("192.168.110.25"));
  for(mode=1;mode<=5;mode++)
    {
      int expected[]={0,-ETIMEDOUT,-EACCES,-EACCES,-E2BIG,-EPROTO};
      assert(agora_control_wake(local,"frame",pcm,sizeof(pcm),sequence,&wake_reply)==expected[mode]);
      assert(!wake_reply);
    }
  mode=0;puts("WAKE_ASR_EXISTING_HTTP_BOUNDS_PASS private-LAN/auth/redirect/timeout/response-size");
  cJSON *session;
  char lease[65];
  assert(agora_control_start(local, &session, lease) == 0 && session);
  cJSON_Delete(session);
  assert(agora_control_keepalive(local, lease) == 0);
  assert(agora_control_stop(local, lease) == 0);
  struct agora_scene_request_s job;
  assert(agora_control_poll(local, lease, &job) == 0 && !job.id[0]);
  mode=20; assert(agora_control_poll(local, lease, &job) == 0);
  assert(!strcmp(job.id,"0123456789abcdef0123456789abcdef") && job.remaining_ms>0);
  for(mode=21;mode<=25;mode++) assert(agora_control_poll(local,lease,&job)==-EPROTO && !job.id[0]);
  mode=26;assert(agora_control_poll(local,lease,&job)==0 && job.id[0] && job.remaining_ms<=25);
  mode=27;assert(agora_control_poll(local,lease,&job)==0 && !job.id[0]);
  /* Equal total delay: host wait preserves budget; network transit expires it. */
  mode=28;assert(agora_control_poll(local,lease,&job)==0 && job.id[0]);
  mode=29;assert(agora_control_poll(local,lease,&job)==0 && !job.id[0]);
  for(mode=30;mode<=35;mode++) assert(agora_control_poll(local,lease,&job)==-EPROTO && !job.id[0]);
  puts("CONTROL_TRANSIT_BUDGET_PASS");
  mode=0;
  unsigned int old = calls;
  assert(agora_control_stop(local, "bad") == -EINVAL && calls == old);
  cJSON_ReplaceItemInObject(local, "server_host", cJSON_CreateString("8.8.8.8"));
  assert(agora_control_start(local, &session, lease) == -EACCES && calls == old);
  cJSON_ReplaceItemInObject(local, "server_host", cJSON_CreateString("192.168.110.25\r\nBad"));
  assert(agora_control_start(local, &session, lease) == -EINVAL && calls == old);
  cJSON_ReplaceItemInObject(local, "server_host", cJSON_CreateString("192.168.110.25"));
  mode = 1; assert(agora_control_start(local, &session, lease) == -ETIMEDOUT);
  mode = 2; assert(agora_control_start(local, &session, lease) == -EACCES);
  mode = 3; assert(agora_control_start(local, &session, lease) == -EACCES);
  mode = 4; assert(agora_control_start(local, &session, lease) == -E2BIG);
  mode = 5; assert(agora_control_start(local, &session, lease) == -EPROTO);
  cJSON_Delete(local);
  const char *config = "{\"app_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"server_host\":\"192.168.110.25\",\"server_port\":8766,\"device_key\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"volume\":55}";
  assert(agora_control_save(config) == 0);
  uint8_t jpeg[] = {255, 216, 0, 1, 255, 217};
  struct vision_badge_image_s image = { .data=jpeg, .size=sizeof(jpeg), .format=VISION_BADGE_IMAGE_JPEG };
  char scene_text[256];
  mode = 0;
  assert(agora_control_scene(&image, scene_text, sizeof(scene_text)) == 0);
  assert(!strcmp(scene_text, "scene-reply"));
  strcpy(job.id,"0123456789abcdef0123456789abcdef");job.remaining_ms=10000;
  cJSON *frame_local=cJSON_Parse(config);assert(frame_local);
  assert(agora_control_scene_request(frame_local,&job,&image)==0);
  assert(agora_control_scene_failed(frame_local,&job)==0);
  mode=7;assert(agora_control_scene_failed(frame_local,&job)==-EPROTO);
  mode=2;assert(agora_control_scene_failed(frame_local,&job)==-EACCES);
  mode=1;assert(agora_control_scene_failed(frame_local,&job)==-ETIMEDOUT);
  mode=0;
  job.id[0]='X';old=calls;
  assert(agora_control_scene_request(frame_local,&job,&image)==-EINVAL && calls==old);
  assert(agora_control_scene_failed(frame_local,&job)==-EINVAL && calls==old);
  job.id[0]='0';job.remaining_ms=0;
  assert(agora_control_scene_request(frame_local,&job,&image)==-EINVAL && calls==old);
  assert(agora_control_scene_failed(frame_local,&job)==-EINVAL && calls==old);
  cJSON_Delete(frame_local);
  assert(agora_control_capture(&image, scene_text, sizeof(scene_text)) == 0);
  assert(!strcmp(scene_text, "scene-reply"));
  mode = 6;
  assert(agora_control_scene(&image, scene_text, sizeof(scene_text)) == -EPROTO && !scene_text[0]);
  mode = 2;
  assert(agora_control_scene(&image, scene_text, sizeof(scene_text)) == -EACCES);
  mode = 0;
  old = calls; image.size = 262145;
  assert(agora_control_scene(&image, scene_text, sizeof(scene_text)) == -EINVAL && calls == old);
  FILE *file = fopen(AGORA_CONFIG_DIR "/agora.cfg", "r");
  assert(file);
  char saved[256] = {0};
  assert(fread(saved, 1, sizeof(saved), file) == strlen(config));
  assert(!strcmp(saved, config));
  assert(!fclose(file));
  assert(agora_control_save("{}") == -EINVAL);
  old = mount_calls;
  storage_mode = 4;
  assert(agora_control_prepare_storage() == -ENOENT && mount_calls == old);
  storage_mode = 5;
  assert(agora_control_prepare_storage() == -EACCES && mount_calls == old);
  storage_mode = 1;
  assert(agora_control_save(config) == -EIO);
  storage_mode = 2;
  assert(agora_control_prepare_storage() == -EBUSY);
  storage_mode = 7;
  assert(agora_control_prepare_storage() == -ENOTDIR);
  storage_mode = 6;
  old = mount_calls;
  assert(agora_control_prepare_storage() == 0 && mount_calls == old);
  storage_mode = 8;
  mount_calls = 0;
  assert(agora_control_prepare_storage() == 0 && mount_calls == 1);
  storage_mode = 3;
  assert(agora_control_prepare_storage() == 0);
  storage_mode = 0;
  assert(agora_control_save(config) == 0);

  /* Complete persistent-config sequence. A successful mount changes the
   * next statfs result, as on NuttX; the second mount would return ENOTDIR.
   * Save/load/resave/reload must never need that second mount. */
  storage_mode = 9; storage_mounted = false; mount_calls = 0;
  assert(agora_control_save(config) == 0 && mount_calls == 1);
  cJSON *loaded = NULL;
  assert(load_local(&loaded) == 0 && cJSON_IsObject(loaded));
  assert(cJSON_GetObjectItemCaseSensitive(loaded, "volume")->valueint == 55);
  cJSON_ReplaceItemInObjectCaseSensitive(loaded, "volume", cJSON_CreateNumber(17));
  char *updated = cJSON_PrintUnformatted(loaded); assert(updated);
  cJSON_Delete(loaded); loaded = NULL;
  assert(agora_control_save(updated) == 0); free(updated);
  assert(load_local(&loaded) == 0);
  assert(cJSON_GetObjectItemCaseSensitive(loaded, "volume")->valueint == 17);
  cJSON_Delete(loaded); loaded = NULL;
  assert(mount_calls == 1);
  /* Failed saves must preserve the old file and report only the first I/O
   * stage/error, not the credential JSON. Hardware itself remains mocked. */
  const char *steps[] = {"write", "flush", "sync", "close", "rename"};
  for (int fault=1;fault<=5;fault++)
    {
      unsigned int diagnostics=save_diagnostics;
      save_fail=fault;
      assert(agora_control_save(config)==-EIO);
      assert(save_diagnostics==diagnostics+1);
      char expected[64];snprintf(expected,sizeof(expected),"stage=%s ret=-5",steps[fault-1]);
      assert(strstr(last_save_diagnostic,expected));
      assert(!strstr(last_save_diagnostic,"aaaaaaaa") && !strstr(last_save_diagnostic,"device_key"));
      save_fail=0;
      assert(access(AGORA_CONFIG_DIR "/agora.tmp",F_OK)<0);
      assert(load_local(&loaded)==0);
      assert(cJSON_GetObjectItemCaseSensitive(loaded,"volume")->valueint==17);
      cJSON_Delete(loaded);loaded=NULL;
    }
  storage_mounted = false; /* Reboot: remount, but reuse the saved file. */
  assert(load_local(&loaded) == 0 && mount_calls == 2);
  assert(cJSON_GetObjectItemCaseSensitive(loaded, "volume")->valueint == 17);
  cJSON_Delete(loaded); loaded = NULL;
  storage_mode = 10; /* Existing wrong filesystem is not accepted. */
  assert(load_local(&loaded) == -ENOTDIR && loaded == NULL);
  storage_mode = 1;
  assert(load_local(&loaded) == -EIO && loaded == NULL);
  storage_mode = 6;
  unsigned int level=99;
  assert(agora_control_volume_load(NULL)==-EINVAL);
  assert(agora_control_volume_load(&level)==-ENOENT && level==99);
  assert(agora_control_volume_save(11)==-EINVAL);
  for(unsigned int next=0;next<=10;next++) {
    assert(agora_control_volume_save(next)==0);
    level=99;assert(agora_control_volume_load(&level)==0 && level==next);
  }
  for(int fault=1;fault<=5;fault++) {
    save_fail=fault;assert(agora_control_volume_save(2)==-EIO);save_fail=0;
    level=99;assert(agora_control_volume_load(&level)==0 && level==10);
    assert(access(AGORA_CONFIG_DIR "/volume.tmp",F_OK)<0);
  }
  const char *bad_volumes[]={"R1V1 11\n","R1V1 -1\n","R1V1 99\n","R1V2 02\n","R1V1 2\n","R1V1 02\nextra",""};
  for(unsigned int i=0;i<sizeof(bad_volumes)/sizeof(bad_volumes[0]);i++) {
    FILE *volume_file=fopen(AGORA_CONFIG_DIR "/volume.cfg","w");assert(volume_file);
    assert(fputs(bad_volumes[i],volume_file)>=0 && fclose(volume_file)==0);
    level=99;assert(agora_control_volume_load(&level)==-EINVAL && level==99);
  }
  storage_mode=1;assert(agora_control_volume_save(2)==-EIO);
  storage_mode=6;assert(agora_control_volume_save(2)==0);
  level=99;assert(agora_control_volume_load(&level)==0 && level==2);
  assert(!unlink(AGORA_CONFIG_DIR "/volume.cfg"));
  puts("VOLUME_STORAGE_PASS levels0..10/invalid/truncated/oversize/five-write-failures/preserve-old/absent-storage; real files, FAT mocked");
  assert(!unlink(AGORA_CONFIG_DIR "/agora.cfg"));
  assert(load_local(&loaded) == -ENOENT && loaded == NULL);
  puts("Agora control-client host tests PASS: save/load/resave/reload/reboot; five I/O failures preserve old config and log only first stage; filesystem/HTTP mocked");
}
