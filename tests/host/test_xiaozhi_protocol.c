/* SPDX-License-Identifier: Apache-2.0 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <vision_badge/xiaozhi_protocol.h>

static int parse(struct xiaozhi_session_s *session, const char *json,
                 enum xiaozhi_event_e *event)
{
  return xiaozhi_parse_control(session, json, strlen(json), event);
}

int main(void)
{
  struct xiaozhi_session_s session;
  enum xiaozhi_event_e event;
  char output[512];
  const char *ack = "{\"type\":\"hello\",\"transport\":\"websocket\","
                    "\"session_id\":\"session-1\",\"audio_params\":{"
                    "\"format\":\"opus\",\"sample_rate\":24000,"
                    "\"channels\":1,\"frame_duration\":60}}";

  xiaozhi_session_reset(&session);
  assert(!session.ready);
  assert(xiaozhi_build_hello(output, sizeof(output)) > 0);
  assert(strstr(output, "16000") != NULL);
  assert(strstr(output, "aec") == NULL);
  assert(xiaozhi_build_hello(output, 1) == -ENOSPC);
  assert(xiaozhi_build_hello(NULL, sizeof(output)) == -EINVAL);
  assert(xiaozhi_build_listen(&session, true, output, sizeof(output)) == -ENOTCONN);
  assert(parse(&session, "{\"type\":\"tts\",\"state\":\"start\"}", &event) == -ENOTCONN);
  assert(parse(&session, "{}junk", &event) == -EBADMSG);
  assert(parse(&session, "{", &event) == -EBADMSG);
  assert(parse(&session, "[]", &event) == -EBADMSG);
  assert(xiaozhi_parse_control(&session, "{}\0x", 4, &event) == -EBADMSG);
  assert(xiaozhi_parse_control(&session, "{}", 0, &event) == -EBADMSG);
  assert(parse(&session, ack, &event) == 0);
  assert(event == XIAOZHI_EVENT_HELLO && session.output_rate == 24000);
  assert(parse(&session, ack, &event) == -EALREADY);
  assert(xiaozhi_build_listen(&session, true, output, sizeof(output)) > 0);
  assert(strstr(output, "session-1") != NULL && strstr(output, "auto") != NULL);
  assert(xiaozhi_build_listen(&session, false, output, sizeof(output)) > 0);
  assert(strstr(output, "stop") != NULL);
  assert(xiaozhi_build_abort(&session, output, 1) == -ENOSPC);
  assert(xiaozhi_build_abort(&session, output, sizeof(output)) > 0);
  assert(strstr(output, "abort") != NULL);
  assert(parse(&session, "{\"type\":\"tts\",\"state\":\"start\",\"session_id\":\"old\"}", &event) == -ESTALE);
  assert(event == XIAOZHI_EVENT_IGNORE);
  assert(parse(&session, "{\"type\":\"tts\",\"state\":\"start\"}", &event) == 0);
  assert(event == XIAOZHI_EVENT_TTS_START);
  assert(parse(&session, "{\"type\":\"tts\",\"state\":\"sentence_start\"}", &event) == 0);
  assert(event == XIAOZHI_EVENT_TTS_SENTENCE);
  assert(parse(&session, "{\"type\":\"tts\",\"state\":\"stop\"}", &event) == 0);
  assert(event == XIAOZHI_EVENT_TTS_STOP);
  assert(parse(&session, "{\"type\":\"tts\",\"state\":\"invalid\"}", &event) == -EPROTO);
  assert(parse(&session, "{\"type\":\"stt\",\"text\":\"你好\"}", &event) == 0);
  assert(event == XIAOZHI_EVENT_STT);
  assert(parse(&session, "{\"type\":\"llm\",\"emotion\":\"happy\"} \n", &event) == 0);
  assert(event == XIAOZHI_EVENT_EMOTION);
  xiaozhi_session_reset(&session);
  assert(parse(&session, "{\"type\":\"hello\",\"transport\":\"websocket\",\"session_id\":\"s\","
                         "\"audio_params\":{\"channels\":true}}", &event) == -EPROTO);
  assert(!session.ready);
  assert(parse(&session, "{\"type\":\"hello\",\"transport\":\"websocket\",\"session_id\":\"s\","
                         "\"audio_params\":{\"sample_rate\":48000}}", &event) == -EPROTO);
  assert(!session.ready);
  assert(parse(&session, "{\"type\":\"hello\",\"transport\":\"websocket\",\"session_id\":\"s\"}", &event) == 0);
  assert(session.output_rate == 16000);
  puts("xiaozhi C protocol: PASS (host only, no board audio or WebSocket transport)");
  return 0;
}
