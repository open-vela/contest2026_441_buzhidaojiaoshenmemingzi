/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef __NuttX__
#  include <netutils/cJSON.h>
#else
#  include <cJSON.h>
#endif

#include <vision_badge/xiaozhi_protocol.h>

static bool string_is(const cJSON *object, const char *key, const char *value)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) && strcmp(item->valuestring, value) == 0;
}

static bool optional_number(const cJSON *object, const char *key, int value)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return item == NULL || (cJSON_IsNumber(item) && item->valuedouble == value);
}

void xiaozhi_session_reset(struct xiaozhi_session_s *session)
{
  if (session != NULL)
    {
      memset(session, 0, sizeof(*session));
    }
}

int xiaozhi_build_hello(char *output, size_t capacity)
{
  static const char message[] =
    "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
    "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
    "\"channels\":1,\"frame_duration\":60}}";

  if (output == NULL)
    {
      return -EINVAL;
    }

  if (capacity < sizeof(message))
    {
      return -ENOSPC;
    }

  memcpy(output, message, sizeof(message));
  return sizeof(message) - 1;
}

static int build_command(const struct xiaozhi_session_s *session,
                         const char *type, const char *state,
                         char *output, size_t capacity)
{
  cJSON *message;
  int ret = -ENOMEM;

  if (session == NULL || output == NULL || capacity > INT_MAX)
    {
      return -EINVAL;
    }

  if (!session->ready)
    {
      return -ENOTCONN;
    }

  message = cJSON_CreateObject();
  if (message == NULL)
    {
      return -ENOMEM;
    }

  if (cJSON_AddStringToObject(message, "session_id", session->id) == NULL ||
      cJSON_AddStringToObject(message, "type", type) == NULL)
    {
      goto out;
    }

  if (state != NULL &&
      (cJSON_AddStringToObject(message, "state", state) == NULL ||
       cJSON_AddStringToObject(message, "mode", "auto") == NULL))
    {
      goto out;
    }

  ret = cJSON_PrintPreallocated(message, output, (int)capacity, false) ?
        (int)strlen(output) : -ENOSPC;
out:
  cJSON_Delete(message);
  return ret;
}

int xiaozhi_build_listen(const struct xiaozhi_session_s *session,
                        bool start, char *output, size_t capacity)
{
  return build_command(session, "listen", start ? "start" : "stop",
                       output, capacity);
}

int xiaozhi_build_abort(const struct xiaozhi_session_s *session,
                       char *output, size_t capacity)
{
  return build_command(session, "abort", NULL, output, capacity);
}

static int accept_hello(struct xiaozhi_session_s *session, const cJSON *message)
{
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(message, "session_id");
  const cJSON *params = cJSON_GetObjectItemCaseSensitive(message, "audio_params");
  const cJSON *rate;
  unsigned int output_rate = 16000;

  if (session->ready)
    {
      return -EALREADY;
    }

  if (!string_is(message, "transport", "websocket") ||
      !cJSON_IsString(id) || id->valuestring[0] == '\0' ||
      strlen(id->valuestring) >= sizeof(session->id))
    {
      return -EPROTO;
    }

  if (params != NULL)
    {
      if (!cJSON_IsObject(params) ||
          (cJSON_GetObjectItemCaseSensitive(params, "format") != NULL &&
           !string_is(params, "format", "opus")) ||
          !optional_number(params, "channels", 1) ||
          !optional_number(params, "frame_duration", 60))
        {
          return -EPROTO;
        }

      rate = cJSON_GetObjectItemCaseSensitive(params, "sample_rate");
      if (rate != NULL)
        {
          if (!cJSON_IsNumber(rate) ||
              (rate->valuedouble != 16000 && rate->valuedouble != 24000))
            {
              return -EPROTO;
            }

          output_rate = (unsigned int)rate->valuedouble;
        }
    }

  memcpy(session->id, id->valuestring, strlen(id->valuestring) + 1);
  session->output_rate = output_rate;
  session->ready = true;
  return 0;
}

int xiaozhi_parse_control(struct xiaozhi_session_s *session,
                         const char *input, size_t length,
                         enum xiaozhi_event_e *event)
{
  const char *end;
  const cJSON *id;
  cJSON *message;
  int ret = 0;

  if (session == NULL || input == NULL || event == NULL)
    {
      return -EINVAL;
    }

  *event = XIAOZHI_EVENT_IGNORE;
  if (length == 0 || length > XIAOZHI_CONTROL_MAX_SIZE ||
      memchr(input, '\0', length) != NULL)
    {
      return -EBADMSG;
    }

  message = cJSON_ParseWithLengthOpts(input, length, &end, false);
  if (!cJSON_IsObject(message))
    {
      cJSON_Delete(message);
      return -EBADMSG;
    }

  while (end < input + length &&
         (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t'))
    {
      end++;
    }

  if (end != input + length)
    {
      ret = -EBADMSG;
      goto out;
    }

  if (string_is(message, "type", "hello"))
    {
      ret = accept_hello(session, message);
      if (ret == 0)
        {
          *event = XIAOZHI_EVENT_HELLO;
        }

      goto out;
    }

  if (!session->ready)
    {
      ret = -ENOTCONN;
      goto out;
    }

  id = cJSON_GetObjectItemCaseSensitive(message, "session_id");
  if (id != NULL && (!cJSON_IsString(id) || strcmp(id->valuestring, session->id) != 0))
    {
      ret = -ESTALE;
      goto out;
    }

  if (string_is(message, "type", "stt"))
    {
      *event = XIAOZHI_EVENT_STT;
    }
  else if (string_is(message, "type", "llm"))
    {
      *event = XIAOZHI_EVENT_EMOTION;
    }
  else if (string_is(message, "type", "tts"))
    {
      if (string_is(message, "state", "start"))
        {
          *event = XIAOZHI_EVENT_TTS_START;
        }
      else if (string_is(message, "state", "sentence_start"))
        {
          *event = XIAOZHI_EVENT_TTS_SENTENCE;
        }
      else if (string_is(message, "state", "stop"))
        {
          *event = XIAOZHI_EVENT_TTS_STOP;
        }
      else
        {
          ret = -EPROTO;
        }
    }

out:
  cJSON_Delete(message);
  return ret;
}
