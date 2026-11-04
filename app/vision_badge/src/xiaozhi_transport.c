/* SPDX-License-Identifier: Apache-2.0 */
/* libwebsockets 4.3.1: reuse its framing, masking, ping and TLS handling.
 * Reference: upstream minimal-ws-client (CC0), not ESP-IDF tasks.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libwebsockets.h>
#include <nuttx/mutex.h>
#include <vision_badge/xiaozhi_audio.h>
#include <vision_badge/xiaozhi_protocol.h>
#include <vision_badge/xiaozhi_transport.h>

#define TX_FRAMES 24
#define CONNECT_MS 10000
struct wire_packet_s
{
  bool binary;
  size_t length;
  uint8_t data[LWS_PRE + XIAOZHI_OPUS_MAX];
};

struct xiaozhi_transport_s
{
  mutex_t lock;
  struct lws_context *context;
  struct lws *socket;
  lws_sorted_usec_list_t tick;
  struct xiaozhi_connection_s config;
  xiaozhi_wire_cb_t receive;
  void *arg;
  bool connected;
  bool closed;
  bool cancelled;
  bool pending;
  bool binary;
  int error;
  uint64_t deadline;
  unsigned int head;
  unsigned int queued;
  size_t rx_length;
  uint8_t rx[XIAOZHI_CONTROL_MAX_SIZE + 1];
  struct wire_packet_s tx[TX_FRAMES];
};

static uint64_t now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void mark_closed(struct xiaozhi_transport_s *wire, int error)
{
  bool notify;
  nxmutex_lock(&wire->lock);
  notify = !wire->closed;
  wire->closed = true;
  wire->connected = false;
  wire->socket = NULL;
  wire->queued = 0;
  if (wire->error == 0)
    {
      wire->error = error;
    }
  nxmutex_unlock(&wire->lock);
  if (notify)
    {
      wire->receive(wire->arg, XIAOZHI_WIRE_CLOSED, NULL, 0);
    }
}

static int receive_message(struct xiaozhi_transport_s *wire)
{
  int ret = wire->receive(wire->arg, wire->binary ? XIAOZHI_WIRE_BINARY :
                         XIAOZHI_WIRE_TEXT, wire->rx, wire->rx_length);
  if (ret == -EAGAIN)
    {
      wire->pending = true;
      lws_rx_flow_control(wire->socket, 0);
      return 0;
    }
  if (ret < 0)
    {
      wire->error = ret;
      return -1;
    }
  wire->rx_length = 0;
  return 0;
}

static int add_header(struct lws *wsi, const char *name, const char *value,
                      unsigned char **p, unsigned char *end)
{
  return lws_add_http_header_by_name(wsi, (const unsigned char *)name,
         (const unsigned char *)value, (int)strlen(value), p, end);
}

static int wire_callback(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user, void *in, size_t length)
{
  struct xiaozhi_transport_s *wire;
  struct wire_packet_s *packet;
  unsigned char **p;
  unsigned char *end;
  char auth[sizeof(wire->config.token) + 8];
  int ret;
  (void)user;
  wire = lws_context_user(lws_get_context(wsi));
  if (wire == NULL)
    {
      return 0;
    }
  switch (reason)
    {
      case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
        p = in;
        end = *p + length;
        if (add_header(wsi, "device-id:", wire->config.device, p, end) ||
            add_header(wsi, "client-id:", wire->config.device, p, end) ||
            add_header(wsi, "protocol-version:", "1", p, end))
          {
            return -1;
          }
        if (wire->config.token[0] != '\0')
          {
            memcpy(auth, "Bearer ", 7);
            strcpy(auth + 7, wire->config.token);
            ret = add_header(wsi, "authorization:", auth, p, end);
            memset(auth, 0, sizeof(auth));
            return ret ? -1 : 0;
          }
        break;
      case LWS_CALLBACK_CLIENT_ESTABLISHED:
        nxmutex_lock(&wire->lock);
        wire->socket = wsi;
        wire->connected = true;
        nxmutex_unlock(&wire->lock);
        ret = wire->receive(wire->arg, XIAOZHI_WIRE_OPEN, NULL, 0);
        if (ret < 0)
          {
            wire->error = ret;
            return -1;
          }
        break;
      case LWS_CALLBACK_CLIENT_RECEIVE:
        if (wire->pending)
          {
            wire->error = -EOVERFLOW;
            return -1;
          }
        if (wire->rx_length == 0)
          {
            wire->binary = lws_frame_is_binary(wsi);
          }
        if (length > sizeof(wire->rx) - 1 - wire->rx_length ||
            (wire->binary && length + wire->rx_length > XIAOZHI_OPUS_MAX))
          {
            wire->error = -EMSGSIZE;
            return -1;
          }
        memcpy(wire->rx + wire->rx_length, in, length);
        wire->rx_length += length;
        if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0)
          {
            return receive_message(wire);
          }
        break;
      case LWS_CALLBACK_CLIENT_WRITEABLE:
        nxmutex_lock(&wire->lock);
        if (wire->queued != 0)
          {
            packet = &wire->tx[wire->head];
            ret = lws_write(wsi, packet->data + LWS_PRE, packet->length,
                            packet->binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT);
            if (ret != (int)packet->length)
              {
                wire->error = -EIO;
                nxmutex_unlock(&wire->lock);
                return -1;
              }
            wire->head = (wire->head + 1) % TX_FRAMES;
            wire->queued--;
          }
        if (wire->queued != 0)
          {
            lws_callback_on_writable(wsi);
          }
        nxmutex_unlock(&wire->lock);
        break;
      case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
        nxmutex_lock(&wire->lock);
        if (wire->connected && wire->queued != 0)
          {
            lws_callback_on_writable(wire->socket);
          }
        nxmutex_unlock(&wire->lock);
        break;
      case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      case LWS_CALLBACK_CLIENT_CLOSED:
        /* Do not echo library failure input: it may include request details. */
        mark_closed(wire, -ECONNRESET);
        break;
      default:
        break;
    }
  return 0;
}

static const struct lws_protocols protocols[] =
{
  { "r1-xiaozhi", wire_callback, 0, 2048, 0, NULL, 0 },
  LWS_PROTOCOL_LIST_TERM
};

static const lws_retry_bo_t idle_policy =
{
  .secs_since_valid_ping = 10,
  .secs_since_valid_hangup = 30
};

static void timer_tick(lws_sorted_usec_list_t *tick)
{
  struct xiaozhi_transport_s *wire =
    lws_container_of(tick, struct xiaozhi_transport_s, tick);
  lws_sul_schedule(wire->context, 0, tick, timer_tick, 50000);
}

static bool safe_header(const char *value, size_t size)
{
  return memchr(value, '\0', size) != NULL &&
         strpbrk(value, "\r\n") == NULL;
}

int xiaozhi_transport_open(struct xiaozhi_transport_s **result,
                          const struct xiaozhi_connection_s *config,
                          xiaozhi_wire_cb_t receive, void *arg)
{
  struct lws_context_creation_info info;
  struct lws_client_connect_info connect;
  struct xiaozhi_transport_s *wire;
  int ret;
  if (result == NULL || config == NULL || receive == NULL ||
      config->port == 0 || config->port > 65535 ||
      !safe_header(config->host, sizeof(config->host)) ||
      !safe_header(config->path, sizeof(config->path)) ||
      !safe_header(config->token, sizeof(config->token)) ||
      !safe_header(config->device, sizeof(config->device)) ||
      !safe_header(config->ca_file, sizeof(config->ca_file)) ||
      config->host[0] == '\0' || config->path[0] != '/' ||
      config->device[0] == '\0' || (config->tls && config->ca_file[0] == '\0'))
    {
      return -EINVAL;
    }
  *result = NULL;
  wire = calloc(1, sizeof(*wire));
  if (wire == NULL)
    {
      return -ENOMEM;
    }
  ret = nxmutex_init(&wire->lock);
  if (ret < 0)
    {
      free(wire);
      return ret;
    }
  wire->config = *config;
  wire->receive = receive;
  wire->arg = arg;
  wire->deadline = now_ms() + CONNECT_MS;
  memset(&info, 0, sizeof(info));
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = protocols;
  info.user = wire;
  info.fd_limit_per_thread = 4;
  if (config->tls)
    {
      info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
#if defined(LWS_WITH_TLS)
      info.client_ssl_ca_filepath = config->ca_file;
#else
      nxmutex_destroy(&wire->lock);
      free(wire);
      return -ENOTSUP;
#endif
    }
  wire->context = lws_create_context(&info);
  if (wire->context == NULL)
    {
      nxmutex_destroy(&wire->lock);
      free(wire);
      return -ENOMEM;
    }
  memset(&connect, 0, sizeof(connect));
  connect.context = wire->context;
  connect.address = config->host;
  connect.host = config->host;
  connect.origin = config->host;
  connect.port = config->port;
  connect.path = config->path;
  connect.local_protocol_name = protocols[0].name;
  connect.pwsi = &wire->socket;
  connect.ssl_connection = config->tls ? LCCSCF_USE_SSL : 0;
  connect.retry_and_idle_policy = &idle_policy;
  if (lws_client_connect_via_info(&connect) == NULL)
    {
      xiaozhi_transport_close(wire);
      return -ECONNREFUSED;
    }
  lws_sul_schedule(wire->context, 0, &wire->tick, timer_tick, 50000);
  *result = wire;
  return 0;
}

int xiaozhi_transport_service(struct xiaozhi_transport_s *wire)
{
  bool connected;
  int ret;
  if (wire == NULL)
    {
      return -EINVAL;
    }
  nxmutex_lock(&wire->lock);
  ret = wire->cancelled ? -ECANCELED : wire->error;
  connected = wire->connected;
  nxmutex_unlock(&wire->lock);
  if (ret < 0)
    {
      return ret;
    }
  if (!connected && now_ms() >= wire->deadline)
    {
      return -ETIMEDOUT;
    }
  if (wire->pending)
    {
      ret = wire->receive(wire->arg, wire->binary ? XIAOZHI_WIRE_BINARY :
                         XIAOZHI_WIRE_TEXT, wire->rx, wire->rx_length);
      if (ret != -EAGAIN)
        {
          if (ret < 0)
            {
              return ret;
            }
          wire->pending = false;
          wire->rx_length = 0;
          lws_rx_flow_control(wire->socket, 1);
        }
    }
  ret = lws_service(wire->context, 0);
  return ret < 0 ? -EIO : 0;
}

int xiaozhi_transport_send(struct xiaozhi_transport_s *wire,
                          bool binary, const void *data, size_t length)
{
  struct wire_packet_s *packet;
  int ret = 0;
  if (wire == NULL || data == NULL || length == 0 || length > XIAOZHI_OPUS_MAX)
    {
      return -EINVAL;
    }
  nxmutex_lock(&wire->lock);
  if (!wire->connected || wire->cancelled)
    {
      ret = -ENOTCONN;
    }
  else if (wire->queued == TX_FRAMES)
    {
      ret = -EAGAIN;
    }
  else
    {
      packet = &wire->tx[(wire->head + wire->queued) % TX_FRAMES];
      packet->binary = binary;
      packet->length = length;
      memcpy(packet->data + LWS_PRE, data, length);
      wire->queued++;
      lws_cancel_service(wire->context);
    }
  nxmutex_unlock(&wire->lock);
  return ret;
}

void xiaozhi_transport_cancel(struct xiaozhi_transport_s *wire)
{
  if (wire != NULL)
    {
      nxmutex_lock(&wire->lock);
      wire->cancelled = true;
      lws_cancel_service(wire->context);
      nxmutex_unlock(&wire->lock);
    }
}

void xiaozhi_transport_close(struct xiaozhi_transport_s *wire)
{
  if (wire != NULL)
    {
      lws_sul_cancel(&wire->tick);
      lws_context_destroy(wire->context);
      nxmutex_destroy(&wire->lock);
      memset(&wire->config, 0, sizeof(wire->config));
      free(wire);
    }
}
