/* SPDX-License-Identifier: Apache-2.0 */
/* Host-only adapter: real agora_control.c, loopback HTTP instead of NuttX.
 * This executable never opens hardware or reads an operator configuration. */
#include <vision_badge/agora_control.h>
#include <netutils/webclient.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static unsigned short server_port;

void webclient_set_defaults(struct webclient_context *ctx)
{ memset(ctx, 0, sizeof(*ctx)); }

void webclient_set_static_body(struct webclient_context *ctx,
                               const void *body, size_t size)
{ ctx->test_body = body; ctx->test_bodylen = size; }

static int send_all(int fd, const void *data, size_t length)
{
  const unsigned char *cursor = data;
  while (length)
    {
      ssize_t count = send(fd, cursor, length, 0);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return -EIO;
      cursor += count;
      length -= count;
    }
  return 0;
}

int webclient_perform(struct webclient_context *ctx)
{
  /* Production control deliberately rejects loopback. Only this transport
   * maps a synthetic private address to 127.0.0.1; production stays intact. */
  const char *prefix = "http://192.168.254.1:";
  assert(!strncmp(ctx->url, prefix, strlen(prefix)));
  const char *path = strchr(ctx->url + strlen(prefix), '/');
  assert(path && !strcmp(ctx->method, "POST"));
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -errno;
  struct timeval timeout = { .tv_sec = 3 };
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
  struct sockaddr_in address = { .sin_family = AF_INET,
    .sin_port = htons(server_port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    { int error = errno; close(fd); return -error; }
  char line[1024];
  int length = snprintf(line, sizeof(line),
      "POST %s HTTP/1.1\r\nHost: localhost\r\nContent-Length: %zu\r\nConnection: close\r\n",
      path, ctx->test_bodylen);
  assert(length > 0 && length < (int)sizeof(line));
  int ret = send_all(fd, line, (size_t)length);
  for (unsigned int i = 0; ret == 0 && i < ctx->nheaders; i++)
    {
      ret = send_all(fd, ctx->headers[i], strlen(ctx->headers[i]));
      if (ret == 0) ret = send_all(fd, "\r\n", 2);
    }
  if (ret == 0) ret = send_all(fd, "\r\n", 2);
  if (ret == 0) ret = send_all(fd, ctx->test_body, ctx->test_bodylen);
  char response[8192];
  size_t used = 0;
  while (ret == 0)
    {
      ssize_t count = recv(fd, response + used, sizeof(response) - used - 1, 0);
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) { ret = -errno; break; }
      if (count == 0) break;
      used += (size_t)count;
      if (used == sizeof(response) - 1) { ret = -E2BIG; break; }
    }
  close(fd);
  if (ret < 0) return ret;
  response[used] = 0;
  assert(sscanf(response, "HTTP/1.1 %u", &ctx->http_status) == 1);
  char *body = strstr(response, "\r\n\r\n");
  assert(body);
  body += 4;
  int capacity = (int)(used - (size_t)(body - response));
  return ctx->sink_callback(&body, 0, capacity, &capacity, ctx->sink_callback_arg);
}

int main(int argc, char **argv)
{
  assert(argc == 3);
  signal(SIGPIPE, SIG_IGN);
  unsigned long port = strtoul(argv[1], NULL, 10);
  assert(port > 0 && port <= 65535);
  server_port = (unsigned short)port;
  FILE *file = fopen(argv[2], "rb");
  assert(file);
  unsigned char jpeg[4096];
  size_t size = fread(jpeg, 1, sizeof(jpeg), file);
  assert(size >= 4 && size < sizeof(jpeg) && !ferror(file));
  assert(fclose(file) == 0);
  struct vision_badge_image_s image = {
    .data = jpeg, .size = size, .format = VISION_BADGE_IMAGE_JPEG };
  cJSON *local = cJSON_CreateObject();
  assert(local);
  cJSON_AddStringToObject(local, "server_host", "192.168.254.1");
  cJSON_AddNumberToObject(local, "server_port", server_port);
  cJSON_AddStringToObject(local, "device_key",
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
  cJSON *session = NULL;
  char lease[65];
  assert(agora_control_start(local, &session, lease) == 0);
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(session, "vision_tools")));
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(session, "vision_long_poll")));
  puts("READY");
  fflush(stdout);
  struct agora_scene_request_s request = {0};
  struct agora_scene_request_s previous = {0};
  char command[32];
  while (fgets(command, sizeof(command), stdin))
    {
      int ret;
      if (!strcmp(command, "poll\n"))
        {
          previous = request;
          ret = agora_control_poll(local, lease, &request);
          printf("POLL %d %d\n", ret, request.id[0] != 0);
        }
      else if (!strcmp(command, "upload\n"))
        printf("UPLOAD %d\n", agora_control_scene_request(local, &request, &image));
      else if (!strcmp(command,"old-upload\n"))
        printf("OLD_UPLOAD %d\n",agora_control_scene_request(local,&previous,&image));
      else if (!strcmp(command,"fresh-frame\n"))
        { jpeg[size-3]^=3;puts("FRESH_FRAME"); }
      else if (!strcmp(command, "wrong-id\n"))
        {
          struct agora_scene_request_s wrong = request;
          wrong.id[0] = wrong.id[0] == 'a' ? 'b' : 'a';
          printf("WRONG_ID %d\n", agora_control_scene_request(local, &wrong, &image));
        }
      else if (!strcmp(command, "stop\n"))
        {
          printf("STOP %d\n", agora_control_stop(local, lease));
          fflush(stdout);
          break;
        }
      else abort();
      fflush(stdout);
    }
  cJSON_Delete(session);
  cJSON_Delete(local);
  return 0;
}
