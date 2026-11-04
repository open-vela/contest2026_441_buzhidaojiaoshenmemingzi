/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VISION_BADGE_XIAOZHI_TRANSPORT_H
#define VISION_BADGE_XIAOZHI_TRANSPORT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct xiaozhi_transport_s;
struct xiaozhi_connection_s
{
  char host[256];
  char path[256];
  char token[256];
  char device[65];
  char ca_file[256];
  unsigned int port;
  bool tls;
};

enum xiaozhi_wire_event_e
{
  XIAOZHI_WIRE_OPEN,
  XIAOZHI_WIRE_TEXT,
  XIAOZHI_WIRE_BINARY,
  XIAOZHI_WIRE_CLOSED
};

/* Receive callback runs only on the service owner. EAGAIN for a complete
 * message pauses receive until the downstream queue has space. Never wait
 * for playback in this callback. CLOSED invalidates all queued old audio.
 */
typedef int (*xiaozhi_wire_cb_t)(void *arg, enum xiaozhi_wire_event_e event,
                                const void *data, size_t length);
int xiaozhi_transport_open(struct xiaozhi_transport_s **transport,
                          const struct xiaozhi_connection_s *config,
                          xiaozhi_wire_cb_t receive, void *arg);
/* Only owner calls service/close. Producers may send/cancel concurrently;
 * join them before close. Queue full is EAGAIN, never a silent packet drop.
 */
int xiaozhi_transport_service(struct xiaozhi_transport_s *transport);
int xiaozhi_transport_send(struct xiaozhi_transport_s *transport,
                          bool binary, const void *data, size_t length);
void xiaozhi_transport_cancel(struct xiaozhi_transport_s *transport);
void xiaozhi_transport_close(struct xiaozhi_transport_s *transport);
#endif
