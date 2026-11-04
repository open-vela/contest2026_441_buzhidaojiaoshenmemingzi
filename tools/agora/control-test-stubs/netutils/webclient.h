/* SPDX-License-Identifier: Apache-2.0 */
/* Test fixture for the pinned NuttX webclient ABI; not built into firmware. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define WEBCLIENT_FLAG_TUNNEL 2U
struct webclient_conn_s { bool tls; int sockfd; unsigned int flags; };
enum webclient_protocol_version_e { WEBCLIENT_PROTOCOL_VERSION_HTTP_1_0, WEBCLIENT_PROTOCOL_VERSION_HTTP_1_1 };
struct webclient_context
{
  enum webclient_protocol_version_e protocol_version;
  const char *method, *url, *proxy, *tunnel_target_host;
  uint16_t tunnel_target_port;
  const char * const *proxy_headers;
  unsigned int proxy_nheaders,flags;
  const char * const *headers;
  unsigned int nheaders;
  char *buffer;
  int buflen;
  unsigned int timeout_sec, http_status;
  int (*header_callback)(const char *, bool, void *);
  int (*sink_callback)(char **, int, int, int *, void *);
  void *sink_callback_arg;
  const void *test_body;
  size_t test_bodylen;
};
void webclient_set_defaults(struct webclient_context *);
void webclient_set_static_body(struct webclient_context *, const void *, size_t);
int webclient_perform(struct webclient_context *);
void webclient_get_tunnel(struct webclient_context *,struct webclient_conn_s **);
void webclient_conn_close(struct webclient_conn_s *);
void webclient_conn_free(struct webclient_conn_s *);
