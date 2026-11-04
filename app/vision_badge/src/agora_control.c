/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <netutils/webclient.h>
#include <vision_badge/agora_control.h>
#include <errno.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#if defined(__NuttX__) && defined(CONFIG_BK7258_WIFI_VNET) && defined(CONFIG_BK7258_AP_CORE)
#  include <arch/chip/bk7258_wifi.h>
#endif
#if defined(__NuttX__) && defined(CONFIG_BK7258_AP_CORE)
#  include <sys/mount.h>
#  include <sys/statfs.h>
#endif

#ifndef AGORA_CONFIG_DIR
#  define AGORA_CONFIG_DIR "/mnt/sdnand/prov"
#endif

struct response_s { char text[4096]; size_t used; uint64_t received_ms; };

/* Read the AP's public NuttX mempool counters, never CP /proc or private
 * kernel addresses. A post-return snapshot cannot prove the failure site.
 * The fixed upstream mempool format is name: total bsize nused nfree ... . */
static inline int callback_free(FILE *stream)
{
  static const char target[] = "g_cbprealloc:";
  char line[160];
  size_t matched = 0;
  unsigned long total, size, used, available;
  /* Upstream's 80-byte row buffer can truncate the newline of longer pool
   * names. Find this exact label in a bounded stream, not by line start. */
  for (unsigned int i = 0; i < 4096; i++)
    {
      int c = fgetc(stream);
      if (c == EOF) break;
      matched = c == target[matched] ? matched + 1 : c == target[0] ? 1 : 0;
      if (matched == sizeof(target) - 1)
        {
          if (fgets(line, sizeof(line), stream) &&
              sscanf(line, "%lu%lu%lu%lu", &total, &size, &used, &available) == 4 &&
              size && used <= total / size && available <= total / size &&
              available <= 65535)
            return (int)available;
          break;
        }
    }
  return -1;
}

static int callback_snapshot(void)
{
  int available = -1, saved = errno;
#if defined(__NuttX__) && defined(CONFIG_BK7258_AP_CORE) && defined(CONFIG_FS_PROCFS)
  struct statfs fs;
  /* AP does not run NSH, so its procfs may not have been mounted yet. */
  if (statfs("/proc", &fs) < 0 || fs.f_type != PROCFS_MAGIC)
    (void)mount(NULL, "/proc", "procfs", 0, NULL);
  if (statfs("/proc", &fs) == 0 && fs.f_type == PROCFS_MAGIC)
    {
      FILE *stream = fopen("/proc/mempool", "r");
      if (stream)
        { available = callback_free(stream); fclose(stream); }
    }
#endif
  errno = saved;
  return available;
}

static uint64_t control_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
}

static void control_diagnostic(const char *action, int ret,
                               unsigned int status, uint64_t elapsed,
                               int callbacks_before, int callbacks_after,
                               int64_t body_first_ms, int64_t body_return_ms)
{
  int link = -1, native = -1;
#if defined(__NuttX__) && defined(CONFIG_BK7258_WIFI_VNET) && defined(CONFIG_BK7258_AP_CORE)
  struct bk7258_wifi_result_s state;
  if (bk7258_wifi_read_link(&state) >= 0)
    {
      link = (int)state.link_state;
      native = bk7258_wifi_native_lease_matches(&state);
    }
#endif
  /* One line per failed control request, without URL, auth or JSON.
   * Never retry a session-start POST: the server may already have accepted it. */
  syslog(ret < 0 ? LOG_ERR : LOG_INFO, "agora: control=%s ret=%d http=%u elapsed=%lu ms link=%d native=%d cb_free_before/after=%d/%d body_first=%ld body_return=%ld ms\n",
         action, ret, status, (unsigned long)elapsed, link, native,
         callbacks_before, callbacks_after, (long)body_first_ms, (long)body_return_ms);
}

int agora_control_prepare_storage(void)
{
#if defined(__NuttX__) && defined(CONFIG_BK7258_AP_CORE)
  /* Same device selection as the existing provision/network path. This
   * application does not own an exclusive mount: leave it available for
   * later config reads and do not unmount another worker's filesystem. */
  const char *device = CONFIG_CONTEST2026_441_VISION_BADGE_MIMO_STORAGE_DEVICE;
  struct stat info;
  struct statfs fs;
  /* NuttX reports ENOTDIR for mount over an existing mountpoint, rather
   * than the EBUSY assumed by the old provision path. Check the actual
   * filesystem first; a plain directory is not persistent storage. */
  if (statfs("/mnt/sdnand", &fs) == 0 && fs.f_type == MSDOS_SUPER_MAGIC)
    return 0;
  if (stat("/dev/mmcsd0p0", &info) == 0) device = "/dev/mmcsd0p0";
  else if (stat(device, &info) < 0) return -errno;
  if (mkdir("/mnt", 0777) < 0 && errno != EEXIST) return -errno;
  if (mkdir("/mnt/sdnand", 0777) < 0 && errno != EEXIST) return -errno;
  if (mount(device, "/mnt/sdnand", "vfat", 0, NULL) < 0)
    {
      int error = errno;
      /* Another worker may have mounted it since our first check. */
      if (statfs("/mnt/sdnand", &fs) == 0 && fs.f_type == MSDOS_SUPER_MAGIC)
        return 0;
      return -error;
    }
#endif
  return 0;
}

static int header(const char *line, bool truncated, void *arg)
{
  (void)arg;
  if (truncated) return -E2BIG;
  /* webclient has no per-request redirect switch in the pinned version. */
  return !strncasecmp(line, "Location:", 9) ? -EACCES : 0;
}

static int sink(char **buffer, int offset, int end, int *size, void *arg)
{
  struct response_s *response = arg;
  (void)size;
  if (offset < 0 || end < offset || (size_t)(end - offset) >= sizeof(response->text) - response->used)
    return -E2BIG;
  if (end > offset && !response->received_ms) response->received_ms = control_now_ms();
  memcpy(response->text + response->used, *buffer + offset, end - offset);
  response->used += end - offset;
  response->text[response->used] = 0;
  return 0;
}

static int control_endpoint(const cJSON *local,cJSON **host_out,
                             cJSON **port_out,cJSON **key_out)
{
  cJSON *host = cJSON_GetObjectItemCaseSensitive(local, "server_host");
  cJSON *port = cJSON_GetObjectItemCaseSensitive(local, "server_port");
  cJSON *key = cJSON_GetObjectItemCaseSensitive(local, "device_key");
  struct in_addr address;
  /* The plain-HTTP control endpoint is deliberately restricted to an explicit
   * private IPv4 LAN address, not a public hostname or URL from a redirect.
   */
  if (!cJSON_IsString(host) || inet_pton(AF_INET, host->valuestring, &address) != 1 ||
      !cJSON_IsNumber(port) || port->valuedouble < 1 || port->valuedouble > 65535 ||
      (double)(unsigned int)port->valuedouble != port->valuedouble ||
      !cJSON_IsString(key) || strlen(key->valuestring) != 64)
    return -EINVAL;
  uint32_t ip = ntohl(address.s_addr);
  if ((ip >> 24) != 10 && (ip >> 20) != 0xac1 && (ip >> 16) != 0xc0a8)
    return -EACCES;
  for (size_t i = 0; i < 64; i++)
    if (!((key->valuestring[i] >= '0' && key->valuestring[i] <= '9') ||
          (key->valuestring[i] >= 'a' && key->valuestring[i] <= 'f'))) return -EINVAL;
  *host_out=host; *port_out=port; *key_out=key;
  return 0;
}

#ifdef CONFIG_CONTEST2026_441_WAKE_UDP
int agora_control_wake_udp_target(const cJSON *local,struct sockaddr_in *peer,
                                   unsigned char secret[32])
{
  if (!peer || !secret) return -EINVAL;
  cJSON *host,*port,*key;
  int ret=control_endpoint(local,&host,&port,&key);
  if (ret<0) return ret;
  memset(peer,0,sizeof(*peer));peer->sin_family=AF_INET;
  peer->sin_port=htons((uint16_t)port->valuedouble);
  inet_pton(AF_INET,host->valuestring,&peer->sin_addr);
  for (unsigned int i=0;i<32;i++)
    {
      unsigned int byte=0;
      for (unsigned int j=0;j<2;j++)
        {char c=key->valuestring[i*2+j];byte=byte*16+(c<='9'?c-'0':c-'a'+10);}
      secret[i]=(unsigned char)byte;
    }
  return 0;
}
#endif

#ifdef CONFIG_CONTEST2026_441_WAKE_STREAM
int agora_control_wake_stream(const cJSON *local,struct webclient_conn_s **conn)
{
  if (!conn) return -EINVAL;
  *conn=NULL;
  cJSON *host,*port,*key;
  int ret=control_endpoint(local,&host,&port,&key);
  if (ret<0) return ret;
  char proxy[64],auth[96],buffer[1024];
  snprintf(proxy,sizeof(proxy),"http://%s:%u",host->valuestring,(unsigned)port->valuedouble);
  snprintf(auth,sizeof(auth),"Authorization: Bearer %s",key->valuestring);
  const char *headers[]={auth,"X-R1-Wake-Stream: 1"};
  struct webclient_context http;
  webclient_set_defaults(&http);
  http.method="CONNECT";
  http.protocol_version=WEBCLIENT_PROTOCOL_VERSION_HTTP_1_1;
  http.flags=WEBCLIENT_FLAG_TUNNEL;
  http.proxy=proxy;
  http.proxy_headers=headers;
  http.proxy_nheaders=2;
  http.tunnel_target_host=host->valuestring;
  http.tunnel_target_port=(uint16_t)port->valuedouble;
  http.buffer=buffer; http.buflen=sizeof(buffer); http.timeout_sec=2;
  http.header_callback=header; /* Same redirect/truncation rejection as POST. */
  ret=webclient_perform(&http);
  if (!ret)
    {
      if (http.http_status/100==2)
        {
          unsigned int status=http.http_status;
          webclient_get_tunnel(&http,conn);
          if(status!=200)
            {
              webclient_conn_close(*conn);webclient_conn_free(*conn);*conn=NULL;
              ret=-EPROTO;
            }
        }
      else ret=http.http_status==401?-EACCES:-EREMOTEIO;
    }
  explicit_bzero(auth,sizeof(auth));
  return ret;
}
#endif

static int post_body_ex(const cJSON *local, const char *path,
                     const char *content_type, const void *body,
                     size_t body_size, cJSON **root,
                     const char *request_header, unsigned int timeout_sec,
                     uint64_t *received_ms)
{
  cJSON *host,*port,*key;
  int valid=control_endpoint(local,&host,&port,&key);
  if (valid<0) return valid;
  char url[128], auth[100], buffer[1024];
  snprintf(url, sizeof(url), "http://%s:%u%s", host->valuestring,
           (unsigned int)port->valuedouble, path);
  snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key->valuestring);
  const char *headers[] = {content_type, auth, request_header};
  struct response_s *response = calloc(1, sizeof(*response));
  if (!response) return -ENOMEM;
  struct webclient_context http;
  webclient_set_defaults(&http);
  http.protocol_version = WEBCLIENT_PROTOCOL_VERSION_HTTP_1_1;
  http.method = "POST";
  http.url = url;
  http.headers = headers;
  http.nheaders = request_header ? 3 : 2;
  http.buffer = buffer;
  http.buflen = sizeof(buffer);
  http.timeout_sec = timeout_sec;
  http.header_callback = header;
  http.sink_callback = sink;
  http.sink_callback_arg = response;
  webclient_set_static_body(&http, body, body_size);
  int callbacks_before = callback_snapshot();
  uint64_t started = control_now_ms();
  int ret = webclient_perform(&http);
  uint64_t elapsed = control_now_ms() - started;
  if (ret >= 0)
    {
      if (http.http_status != 200)
        {
          /* A lost UDP-start reply can leave its old local-ASR owner alive
           * until the3s idle lease expires. Only this non-cloud operation
           * may retry429; never turn an ambiguous RTC join into a retry. */
          if (http.http_status==429 && !strcmp(path,"/wake/udp-start")) ret=-EAGAIN;
          else ret=http.http_status==401?-EACCES:-EREMOTEIO;
        }
      else
        {
          *root = cJSON_ParseWithLengthOpts(response->text, response->used + 1, NULL, true);
          if (!*root) ret = -EPROTO;
        }
    }
  if (ret < 0 || elapsed >= 1000)
    {
      int64_t first = response->received_ms ? (int64_t)(response->received_ms - started) : -1;
      /* The second phase includes remaining body reads and webclient close;
       * it is not a separate measurement of socket-close time. */
      int64_t tail = first >= 0 && (uint64_t)first <= elapsed ? (int64_t)elapsed - first : -1;
      control_diagnostic(path, ret, http.http_status, elapsed,
                         callbacks_before, callback_snapshot(), first, tail);
    }
  if (received_ms) *received_ms = response->received_ms;
  explicit_bzero(auth, sizeof(auth));
  explicit_bzero(response, sizeof(*response));
  free(response);
  return ret;
}

static int post_body(const cJSON *local, const char *path,
                     const char *content_type, const void *body,
                     size_t body_size, cJSON **root)
{
  return post_body_ex(local, path, content_type, body, body_size, root, NULL, 30, NULL);
}

static int post(const cJSON *local, const char *path, const char *body, cJSON **root,
                uint64_t *received_ms)
{
  char endpoint[64];
  snprintf(endpoint, sizeof(endpoint), "/session/%s", path);
  return post_body_ex(local, endpoint, "Content-Type: application/json",
                      body, strlen(body), root, NULL, 30, received_ms);
}

int agora_control_load(cJSON **local)
{
  char config[256];
  FILE *file;
  int ret;
  if (!local) return -EINVAL;
  *local = NULL;
  ret = agora_control_prepare_storage();
  if (ret < 0) return ret;
  file = fopen(AGORA_CONFIG_DIR "/agora.cfg", "rb");
  if (!file) return -errno;
  size_t count = fread(config, 1, sizeof(config), file);
  bool bad = ferror(file) || count == sizeof(config);
  if (fclose(file) != 0) bad = true;
  if (bad) ret = -EIO;
  else
    {
      config[count] = 0;
      *local = cJSON_ParseWithLengthOpts(config, count + 1, NULL, true);
      ret = *local ? 0 : -EINVAL;
    }
  explicit_bzero(config, sizeof(config));
  return ret;
}

#ifdef CONFIG_CONTEST2026_441_WAKE_ASR
int agora_control_wake(const cJSON *local, const char *operation,
                        const void *body, size_t bytes, const char *sequence,
                        cJSON **reply)
{
  if (!operation || !body || !reply) return -EINVAL;
  bool frame=!strcmp(operation,"frame");
  if ((!frame && strcmp(operation,"start") && strcmp(operation,"stop") && strcmp(operation,"udp-start")) ||
      (frame && (bytes!=16000 || !sequence)) ||
      (!frame && (!bytes || bytes>128 || sequence))) return -EINVAL;
  char path[24]; snprintf(path,sizeof(path),"/wake/%s",operation);
  return post_body_ex(local,path,frame?"Content-Type: application/octet-stream":
                       "Content-Type: application/json",body,bytes,reply,sequence,2,NULL);
}
#endif

static int control_frame(const struct vision_badge_image_s *image,
                          char *text, size_t capacity, bool capture_only)
{
  cJSON *local = NULL, *reply = NULL;
  if (!image || !image->data || image->size < 4 || image->size > 262144 ||
      image->format != VISION_BADGE_IMAGE_JPEG || !text || capacity < 2)
    return -EINVAL;
  text[0] = 0;
  int ret = agora_control_load(&local);
  uint64_t started = control_now_ms();
  if (ret >= 0)
    ret = post_body(local, capture_only ? "/vision/capture" : "/vision/frame", "Content-Type: image/jpeg",
                     image->data, image->size, &reply);
  if (ret >= 0 && control_now_ms() - started > 15000) ret = -ETIMEDOUT;
  if (ret >= 0)
    {
      cJSON *value = cJSON_GetObjectItemCaseSensitive(reply, "text");
      cJSON *navigation = cJSON_GetObjectItemCaseSensitive(reply, "navigation");
      cJSON *server_ms = cJSON_GetObjectItemCaseSensitive(reply, "elapsed_ms");
      cJSON *model_ms = cJSON_GetObjectItemCaseSensitive(reply, "model_ms");
      if (!cJSON_IsString(value) || !value->valuestring[0] ||
          strlen(value->valuestring) >= capacity || !cJSON_IsFalse(navigation))
        ret = -EPROTO;
      else
        {
          memcpy(text, value->valuestring, strlen(value->valuestring) + 1);
          syslog(LOG_INFO,
                 "vision_badge: scene timing request=%lu server=%d model=%d ms\n",
                 (unsigned long)(control_now_ms() - started),
                 cJSON_IsNumber(server_ms) && server_ms->valuedouble >= 0 &&
                 server_ms->valuedouble <= 15000 ? (int)server_ms->valuedouble : -1,
                 cJSON_IsNumber(model_ms) && model_ms->valuedouble >= 0 &&
                 model_ms->valuedouble <= 15000 ? (int)model_ms->valuedouble : -1);
        }
    }
  cJSON_Delete(reply);
  cJSON_Delete(local);
  return ret;
}

int agora_control_scene(const struct vision_badge_image_s *image,
                         char *text, size_t capacity)
{
  return control_frame(image, text, capacity, false);
}

int agora_control_capture(const struct vision_badge_image_s *image,
                           char *text, size_t capacity)
{
  return control_frame(image, text, capacity, true);
}

int agora_control_start(const cJSON *local, cJSON **session, char lease[65])
{
  if (!local || !session || !lease) return -EINVAL;
  *session = NULL;
  lease[0] = 0;
#if defined(CONFIG_BK7258_AIDK_CAMERA) && defined(CONFIG_BK7258_AP_CORE)
  int ret = post(local, "start", "{\"vision_tool_v1\":true,\"vision_poll_v2\":true}", session, NULL);
#else
  int ret = post(local, "start", "{}", session, NULL);
#endif
  if (ret < 0) return ret;
  cJSON *value = cJSON_GetObjectItemCaseSensitive(*session, "lease");
  if (!cJSON_IsString(value) || strlen(value->valuestring) != 64)
    { cJSON_Delete(*session); *session = NULL; return -EPROTO; }
  for (size_t i = 0; i < 64; i++)
    if (!((value->valuestring[i] >= '0' && value->valuestring[i] <= '9') ||
          (value->valuestring[i] >= 'a' && value->valuestring[i] <= 'f')))
      { cJSON_Delete(*session); *session = NULL; return -EPROTO; }
  memcpy(lease, value->valuestring, 65);
  return 0;
}

static int control(const cJSON *local, const char *lease, const char *action,
                    struct agora_scene_request_s *request)
{
  if (!lease || strlen(lease) != 64) return -EINVAL;
  for (size_t i = 0; i < 64; i++)
    if (!((lease[i] >= '0' && lease[i] <= '9') || (lease[i] >= 'a' && lease[i] <= 'f')))
      return -EINVAL;
  char body[96];
  snprintf(body, sizeof(body), "{\"lease\":\"%s\"}", lease);
  cJSON *reply = NULL;
  uint64_t started = control_now_ms();
  uint64_t received_ms = 0;
  int ret = post(local, action, body, &reply, &received_ms);
  if (ret >= 0 && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok"))) ret = -EPROTO;
  if (ret >= 0 && request)
    {
      cJSON *job = cJSON_GetObjectItemCaseSensitive(reply, "vision_request");
      if (job)
        {
          cJSON *id = cJSON_GetObjectItemCaseSensitive(job, "request_id");
          cJSON *operation = cJSON_GetObjectItemCaseSensitive(job, "action");
          cJSON *ms = cJSON_GetObjectItemCaseSensitive(job, "remaining_ms");
          cJSON *question = cJSON_GetObjectItemCaseSensitive(job, "question");
          if (!cJSON_IsObject(job) || !cJSON_IsString(id) || strlen(id->valuestring) != 32 ||
              !cJSON_IsString(operation) || strcmp(operation->valuestring, "capture_scene") ||
              !cJSON_IsNumber(ms) || ms->valuedouble < 1 || ms->valuedouble > 10000 ||
              (double)(uint32_t)ms->valuedouble != ms->valuedouble ||
              !cJSON_IsString(question) || !question->valuestring[0] ||
              strlen(question->valuestring) > 240)
            ret = -EPROTO;
          else
            {
              for (size_t i = 0; i < 32; i++)
                if (!strchr("0123456789abcdef", id->valuestring[i])) ret = -EPROTO;
              /* A response can already be stale at its first body byte.
               * Remove the host's measured processing/long-poll wait from
               * total RTT, then conservatively charge both network legs.
               * This needs no synchronized clock. Legacy hosts retain the
               * body-age estimate; the host's absolute deadline still wins. */
              uint64_t now = control_now_ms();
              uint64_t elapsed = now - (received_ms ? received_ms : started);
              uint64_t transit = 0;
              cJSON *server = cJSON_GetObjectItemCaseSensitive(reply, "server_elapsed_ms");
              if (server)
                {
                  uint64_t roundtrip = now - started;
                  if (!cJSON_IsNumber(server) ||
                      !(server->valuedouble >= 0 && server->valuedouble <= 30000) ||
                      (double)(uint32_t)server->valuedouble != server->valuedouble ||
                      server->valuedouble > (double)roundtrip + 2)
                    ret = -EPROTO;
                  else
                    {
                      uint32_t waited = (uint32_t)server->valuedouble;
                      transit = roundtrip > waited ? roundtrip - waited : 0;
                      if (transit > elapsed) elapsed = transit;
                    }
                }
              if (ret == 0 && elapsed < (uint32_t)ms->valuedouble)
                {
                  memcpy(request->id, id->valuestring, 33);
                  request->remaining_ms = (uint32_t)ms->valuedouble - elapsed;
                }
              if (ret == 0)
                syslog(LOG_INFO, "agora: scene delivery accepted=%d delivery_budget_used=%lu transit_bound=%lu remaining=%lu ms\n",
                       request->id[0] != 0, (unsigned long)elapsed,
                       (unsigned long)transit,
                       (unsigned long)ms->valuedouble);
              /* Expired delivery is not a new command or a lease failure. */
            }
        }
    }
  cJSON_Delete(reply);
  explicit_bzero(body, sizeof(body));
  return ret;
}
int agora_control_keepalive(const cJSON *local, const char *lease)
{ return control(local, lease, "keepalive", NULL); }
int agora_control_poll(const cJSON *local, const char *lease,
                       struct agora_scene_request_s *request)
{
  if (!request) return -EINVAL;
  memset(request, 0, sizeof(*request));
  return control(local, lease, "keepalive", request);
}
int agora_control_stop(const cJSON *local, const char *lease)
{ return control(local, lease, "stop", NULL); }

static bool scene_request_valid(const struct agora_scene_request_s *request)
{
  if (!request || strnlen(request->id, sizeof(request->id)) != 32 ||
      !request->remaining_ms || request->remaining_ms > 10000) return false;
  for (size_t i = 0; i < 32; i++)
    if (!strchr("0123456789abcdef", request->id[i])) return false;
  return true;
}

int agora_control_scene_failed(const cJSON *local,
                               const struct agora_scene_request_s *request)
{
  if (!local || !scene_request_valid(request)) return -EINVAL;
  static const char body[] = "{\"capture_failed\":true}";
  char correlation[64];
  snprintf(correlation, sizeof(correlation), "X-R1-Vision-Request: %s", request->id);
  cJSON *reply = NULL;
  unsigned int budget = request->remaining_ms < 2000 ? request->remaining_ms : 2000;
  uint64_t started = control_now_ms();
  int ret = post_body_ex(local, "/vision/frame", "Content-Type: application/json",
                         body, sizeof(body) - 1, &reply, correlation,
                         (budget + 999u) / 1000u, NULL);
  if (ret == 0 && control_now_ms() - started >= budget) ret = -ETIMEDOUT;
  if (ret == 0 && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok"))) ret = -EPROTO;
  cJSON_Delete(reply);
  return ret;
}

int agora_control_scene_request(const cJSON *local,
                                const struct agora_scene_request_s *request,
                                const struct vision_badge_image_s *image)
{
  if (!local || !scene_request_valid(request) ||
      !image || !image->data || image->size < 4 || image->size > 262144 ||
      image->format != VISION_BADGE_IMAGE_JPEG) return -EINVAL;
  char correlation[64];
  snprintf(correlation, sizeof(correlation), "X-R1-Vision-Request: %s", request->id);
  cJSON *reply = NULL;
  uint64_t started = control_now_ms();
  int ret = post_body_ex(local, "/vision/frame", "Content-Type: image/jpeg",
                         image->data, image->size, &reply, correlation,
                         (request->remaining_ms + 999u) / 1000u, NULL);
  if (ret == 0 && control_now_ms() - started >= request->remaining_ms) ret = -ETIMEDOUT;
  if (ret == 0)
    {
      cJSON *text = cJSON_GetObjectItemCaseSensitive(reply, "text");
      if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(reply, "navigation")) ||
          !cJSON_IsString(text) || !text->valuestring[0] || strlen(text->valuestring) > 240)
        ret = -EPROTO;
    }
  cJSON_Delete(reply);
  return ret;
}

int agora_control_save(const char *json)
{
  if (!json || strlen(json) > 240) return -EINVAL;
  cJSON *root = cJSON_ParseWithLengthOpts(json, strlen(json) + 1, NULL, true);
  if (!root) return -EINVAL;
  cJSON *app = cJSON_GetObjectItemCaseSensitive(root, "app_id");
  cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "server_host");
  cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "server_port");
  cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "device_key");
  cJSON *volume = cJSON_GetObjectItemCaseSensitive(root, "volume");
  struct in_addr address;
  int ret = -EINVAL;
  const char *step = "validate";
  if (!cJSON_IsString(app) || strlen(app->valuestring) != 32 ||
      !cJSON_IsString(host) || inet_pton(AF_INET, host->valuestring, &address) != 1 ||
      !cJSON_IsNumber(port) || port->valuedouble < 1 || port->valuedouble > 65535 ||
      (double)(unsigned int)port->valuedouble != port->valuedouble ||
      !cJSON_IsString(key) || strlen(key->valuestring) != 64 ||
      !cJSON_IsNumber(volume) || volume->valuedouble < 0 || volume->valuedouble > 100 ||
      (double)(unsigned int)volume->valuedouble != volume->valuedouble) goto out;
  uint32_t ip = ntohl(address.s_addr);
  if ((ip >> 24) != 10 && (ip >> 20) != 0xac1 && (ip >> 16) != 0xc0a8) goto out;
  for (size_t i = 0; i < 32; i++)
    if (!strchr("0123456789abcdefABCDEF", app->valuestring[i])) goto out;
  for (size_t i = 0; i < 64; i++)
    if (!strchr("0123456789abcdef", key->valuestring[i])) goto out;
  if (cJSON_GetArraySize(root) != 5) goto out; /* Reject unknown/duplicate keys. */
  step = "mount";
  ret = agora_control_prepare_storage();
  if (ret < 0) goto out;
  step = "directory";
  if (mkdir(AGORA_CONFIG_DIR, 0700) < 0 && errno != EEXIST) { ret = -errno; goto out; }
  step = "open-temp";
  FILE *file = fopen(AGORA_CONFIG_DIR "/agora.tmp", "w");
  if (!file) { ret = -errno; goto out; }
  step = "write";
  ret = fwrite(json, 1, strlen(json), file) == strlen(json) ? 0 : -EIO;
  if (!ret) { step = "flush"; if (fflush(file)) ret = -errno; }
  if (!ret) { step = "sync"; if (fsync(fileno(file))) ret = -errno; }
  int closed = fclose(file);
  if (closed && !ret) { step = "close"; ret = -errno; }
  if (!ret)
    {
      step = "rename";
      if (rename(AGORA_CONFIG_DIR "/agora.tmp", AGORA_CONFIG_DIR "/agora.cfg")) ret = -errno;
    }
  if (ret) unlink(AGORA_CONFIG_DIR "/agora.tmp");
out:
  /* Preserve the first failure without printing configuration or secrets.
   * Never retry a failed write/rename automatically or remount a live FS. */
  if (ret && strcmp(step, "validate"))
    syslog(LOG_ERR, "agora: config-save stage=%s ret=%d\n", step, ret);
  cJSON_Delete(root);
  return ret;
}

int agora_control_volume_load(unsigned int *level)
{
  if (!level) return -EINVAL;
  int ret = agora_control_prepare_storage();
  if (ret < 0) return ret;
  FILE *file = fopen(AGORA_CONFIG_DIR "/volume.cfg", "r");
  if (!file) return -errno;
  char record[8];
  size_t size = fread(record, 1, sizeof(record), file);
  bool invalid = size != sizeof(record) || fgetc(file) != EOF;
  ret = ferror(file) ? -EIO : 0;
  if (fclose(file) && !ret) ret = -errno;
  if (ret) return ret;
  if (invalid || memcmp(record, "R1V1 ", 5) || record[7] != '\n' ||
      record[5] < '0' || record[5] > '1' || record[6] < '0' || record[6] > '9') return -EINVAL;
  unsigned int value = (record[5] - '0') * 10u + record[6] - '0';
  if (value > 10) return -EINVAL;
  *level = value;
  return 0;
}

int agora_control_volume_save(unsigned int level)
{
  if (level > 10) return -EINVAL;
  int ret = agora_control_prepare_storage();
  if (ret < 0) return ret;
  if (mkdir(AGORA_CONFIG_DIR, 0700) < 0 && errno != EEXIST) return -errno;
  FILE *file = fopen(AGORA_CONFIG_DIR "/volume.tmp", "w");
  if (!file) return -errno;
  char record[9];
  snprintf(record, sizeof(record), "R1V1 %02u\n", level);
  ret = fwrite(record, 1, 8, file) == 8 ? 0 : -EIO;
  if (!ret && fflush(file)) ret = -errno;
  if (!ret && fsync(fileno(file))) ret = -errno;
  if (fclose(file) && !ret) ret = -errno;
  if (!ret && rename(AGORA_CONFIG_DIR "/volume.tmp", AGORA_CONFIG_DIR "/volume.cfg")) ret = -errno;
  if (ret) unlink(AGORA_CONFIG_DIR "/volume.tmp");
  return ret;
}
