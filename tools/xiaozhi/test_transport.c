/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <vision_badge/xiaozhi_protocol.h>
#include <vision_badge/xiaozhi_transport.h>

struct client_s
{
  struct xiaozhi_transport_s *wire;
  struct xiaozhi_session_s session;
  unsigned int binary;
  unsigned int deferred;
  unsigned int closed;
  bool done;
};

static int receive(void *arg, enum xiaozhi_wire_event_e event,
                    const void *data, size_t length)
{
  struct client_s *client = arg;
  enum xiaozhi_event_e control;
  char hello[512];
  int ret;
  if (event == XIAOZHI_WIRE_OPEN)
    {
      ret = xiaozhi_build_hello(hello, sizeof(hello));
      /* During open, pointer may not be published to the caller yet.
       * Establishment happens from service(), not synchronous open().
       */
      assert(client->wire != NULL);
      return xiaozhi_transport_send(client->wire, false, hello, ret);
    }
  if (event == XIAOZHI_WIRE_CLOSED)
    {
      client->closed++;
      return 0;
    }
  if (event == XIAOZHI_WIRE_BINARY)
    {
      if (client->deferred++ % 3 != 2)
        {
          return -EAGAIN;
        }
      assert(length == 301);
      for (size_t i = 0; i < length; i++)
        {
          assert(((const unsigned char *)data)[i] == (unsigned char)client->binary);
        }
      client->binary++;
      return 0;
    }
  ret = xiaozhi_parse_control(&client->session, data, length, &control);
  assert(ret == 0);
  if (control == XIAOZHI_EVENT_HELLO)
    {
      ret = xiaozhi_build_listen(&client->session, true, hello, sizeof(hello));
      assert(ret > 0);
      ret = xiaozhi_transport_send(client->wire, false, hello, ret);
    }
  if (control == XIAOZHI_EVENT_TTS_STOP)
    {
      client->done = true;
    }
  return ret < 0 ? ret : 0;
}

static void *cancel_worker(void *arg)
{
  usleep(100000);
  xiaozhi_transport_cancel(arg);
  return NULL;
}

int main(int argc, char **argv)
{
  struct xiaozhi_connection_s config = {0};
  struct client_s client = {0};
  pthread_t cancel;
  int ret;
  unsigned int phase;
  assert(argc == 2);
  strcpy(config.host, "127.0.0.1");
  strcpy(config.path, "/xiaozhi/v1/");
  strcpy(config.device, "r1-silent-test");
  strcpy(config.token, "test-only");
  config.port = (unsigned int)atoi(argv[1]);
  for (phase = 0; phase < 3; phase++)
    {
      memset(&client, 0, sizeof(client));
      ret = xiaozhi_transport_open(&client.wire, &config, receive, &client);
      assert(ret == 0);
      if (phase == 2)
        {
          assert(pthread_create(&cancel, NULL, cancel_worker, client.wire) == 0);
        }
      do
        {
          ret = xiaozhi_transport_service(client.wire);
        }
      while (ret >= 0 && !client.done);
      if (phase == 0)
        {
          assert(client.done && client.binary == 64);
        }
      else if (phase == 1)
        {
          assert(ret == -EMSGSIZE);
        }
      else
        {
          assert(pthread_join(cancel, NULL) == 0);
          assert(ret == -ECANCELED);
        }
      xiaozhi_transport_close(client.wire);
      assert(client.closed == 1);
    }
  config.token[0] = '\n';
  assert(xiaozhi_transport_open(&client.wire, &config, receive, &client) == -EINVAL);
  puts("WebSocket transport: fragmentation, backpressure, ping, oversized input, restart, cross-thread cancellation passed");
  return 0;
}
