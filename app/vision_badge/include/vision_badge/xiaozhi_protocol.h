/* SPDX-License-Identifier: Apache-2.0 */

#ifndef VISION_BADGE_XIAOZHI_PROTOCOL_H
#define VISION_BADGE_XIAOZHI_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>

#define XIAOZHI_SESSION_CAPACITY 129
#define XIAOZHI_CONTROL_MAX_SIZE 8192

enum xiaozhi_event_e
{
  XIAOZHI_EVENT_IGNORE = 0,
  XIAOZHI_EVENT_HELLO,
  XIAOZHI_EVENT_STT,
  XIAOZHI_EVENT_TTS_START,
  XIAOZHI_EVENT_TTS_SENTENCE,
  XIAOZHI_EVENT_TTS_STOP,
  XIAOZHI_EVENT_EMOTION
};

struct xiaozhi_session_s
{
  bool ready;
  unsigned int output_rate;
  char id[XIAOZHI_SESSION_CAPACITY];
};

/* Single owner: the connection/control task. Call reset after disconnect.
 * Audio workers receive commands through queues; never modify this object.
 */

void xiaozhi_session_reset(struct xiaozhi_session_s *session);
int xiaozhi_build_hello(char *output, size_t capacity);
int xiaozhi_build_listen(const struct xiaozhi_session_s *session,
                        bool start, char *output, size_t capacity);
int xiaozhi_build_abort(const struct xiaozhi_session_s *session,
                       char *output, size_t capacity);

/* Input is one complete WebSocket TEXT message, not a TCP fragment or Opus.
 * Length excludes any terminating NUL. Output event never retains JSON memory.
 * STOP means server finished sending, not local playback queue has drained.
 */

int xiaozhi_parse_control(struct xiaozhi_session_s *session,
                         const char *input, size_t length,
                         enum xiaozhi_event_e *event);

#endif
