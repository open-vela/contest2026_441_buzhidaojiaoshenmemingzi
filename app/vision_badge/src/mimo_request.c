/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vision_badge/mimo_request.h>

#ifndef EOVERFLOW
#  define EOVERFLOW ERANGE
#endif

#define VISION_MIMO_MODEL "mimo-v2.5"

static const char g_body_prefix[] =
  "{\"model\":\"" VISION_MIMO_MODEL "\","
  "\"messages\":["
  "{\"role\":\"system\",\"content\":"
  "\"你是视障辅助助手。请用一到两句简短中文回答，先说障碍或危险，再说主要人物、物体或文字；只描述图片中能确认的内容，总长度不超过60个汉字。\"},"
  "{\"role\":\"user\",\"content\":["
  "{\"type\":\"image_url\",\"image_url\":{\"url\":"
  "\"data:image/jpeg;base64,";

static const char g_body_middle[] =
  "\"}},{\"type\":\"text\",\"text\":";

static const char g_body_end[] =
  "}]}],\"thinking\":{\"type\":\"disabled\"},"
  "\"max_completion_tokens\":256,"
  "\"temperature\":0.2,\"stream\":false}";

static const char g_base64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int vision_json_quote(const char *input, char **output,
                             size_t *output_size)
{
  static const char hex[] = "0123456789abcdef";
  const unsigned char *src = (const unsigned char *)input;
  char *quoted;
  size_t input_size;
  size_t capacity;
  size_t used = 0;

  input_size = strlen(input);
  if (input_size > (SIZE_MAX - 3) / 6)
    {
      return -EOVERFLOW;
    }

  capacity = input_size * 6 + 3;
  quoted = malloc(capacity);
  if (quoted == NULL)
    {
      return -ENOMEM;
    }

  quoted[used++] = '"';
  while (*src != '\0')
    {
      unsigned char ch = *src++;

      switch (ch)
        {
          case '"':
          case '\\':
            quoted[used++] = '\\';
            quoted[used++] = (char)ch;
            break;

          case '\b':
            quoted[used++] = '\\';
            quoted[used++] = 'b';
            break;

          case '\f':
            quoted[used++] = '\\';
            quoted[used++] = 'f';
            break;

          case '\n':
            quoted[used++] = '\\';
            quoted[used++] = 'n';
            break;

          case '\r':
            quoted[used++] = '\\';
            quoted[used++] = 'r';
            break;

          case '\t':
            quoted[used++] = '\\';
            quoted[used++] = 't';
            break;

          default:
            if (ch < 0x20)
              {
                quoted[used++] = '\\';
                quoted[used++] = 'u';
                quoted[used++] = '0';
                quoted[used++] = '0';
                quoted[used++] = hex[ch >> 4];
                quoted[used++] = hex[ch & 0x0f];
              }
            else
              {
                quoted[used++] = (char)ch;
              }
            break;
        }
    }

  quoted[used++] = '"';
  quoted[used] = '\0';
  *output = quoted;
  *output_size = used;
  return 0;
}

static char vision_base64_at(const struct vision_mimo_body_s *body,
                             size_t position)
{
  size_t group = position / 4;
  size_t lane = position % 4;
  size_t offset = group * 3;
  uint32_t value = (uint32_t)body->image[offset] << 16;
  size_t available = body->image_size - offset;

  if (available > 1)
    {
      value |= (uint32_t)body->image[offset + 1] << 8;
    }

  if (available > 2)
    {
      value |= body->image[offset + 2];
    }

  if ((lane == 2 && available < 2) || (lane == 3 && available < 3))
    {
      return '=';
    }

  return g_base64[(value >> (18 - lane * 6)) & 0x3f];
}

int vision_mimo_body_init(struct vision_mimo_body_s *body,
                          const uint8_t *image, size_t image_size,
                          const char *prompt)
{
  char *quoted = NULL;
  size_t quoted_size;
  size_t suffix_size;
  size_t total_size;
  int ret;

  if (body == NULL || image == NULL || image_size == 0 || prompt == NULL ||
      prompt[0] == '\0')
    {
      return -EINVAL;
    }

  memset(body, 0, sizeof(*body));
  if (image_size > (SIZE_MAX / 4) * 3 - 2)
    {
      return -EOVERFLOW;
    }

  ret = vision_json_quote(prompt, &quoted, &quoted_size);
  if (ret < 0)
    {
      return ret;
    }

  if (sizeof(g_body_middle) - 1 > SIZE_MAX - quoted_size ||
      sizeof(g_body_middle) - 1 + quoted_size >
        SIZE_MAX - (sizeof(g_body_end) - 1))
    {
      free(quoted);
      return -EOVERFLOW;
    }

  suffix_size = sizeof(g_body_middle) - 1 + quoted_size +
                sizeof(g_body_end) - 1;
  body->suffix = malloc(suffix_size + 1);
  if (body->suffix == NULL)
    {
      free(quoted);
      return -ENOMEM;
    }

  memcpy(body->suffix, g_body_middle, sizeof(g_body_middle) - 1);
  memcpy(body->suffix + sizeof(g_body_middle) - 1, quoted, quoted_size);
  memcpy(body->suffix + sizeof(g_body_middle) - 1 + quoted_size,
         g_body_end, sizeof(g_body_end));
  free(quoted);

  body->image = image;
  body->image_size = image_size;
  body->suffix_size = suffix_size;
  body->base64_size = ((image_size + 2) / 3) * 4;

  if (sizeof(g_body_prefix) - 1 > SIZE_MAX - body->base64_size ||
      sizeof(g_body_prefix) - 1 + body->base64_size >
        SIZE_MAX - body->suffix_size)
    {
      vision_mimo_body_deinit(body);
      return -EOVERFLOW;
    }

  total_size = sizeof(g_body_prefix) - 1 + body->base64_size +
               body->suffix_size;
  body->total_size = total_size;
  return 0;
}

void vision_mimo_body_deinit(struct vision_mimo_body_s *body)
{
  if (body != NULL)
    {
      free(body->suffix);
      memset(body, 0, sizeof(*body));
    }
}

size_t vision_mimo_body_read(struct vision_mimo_body_s *body,
                             void *buffer, size_t capacity)
{
  uint8_t *out = buffer;
  const size_t prefix_size = sizeof(g_body_prefix) - 1;
  size_t copied = 0;

  if (body == NULL || buffer == NULL || capacity == 0 ||
      body->sent >= body->total_size)
    {
      return 0;
    }

  while (copied < capacity && body->sent < body->total_size)
    {
      if (body->sent < prefix_size)
        {
          size_t left = prefix_size - body->sent;
          size_t take = left < capacity - copied ? left : capacity - copied;

          memcpy(out + copied, g_body_prefix + body->sent, take);
          body->sent += take;
          copied += take;
        }
      else if (body->sent < prefix_size + body->base64_size)
        {
          size_t position = body->sent - prefix_size;

          out[copied++] = (uint8_t)vision_base64_at(body, position);
          body->sent++;
        }
      else
        {
          size_t position = body->sent - prefix_size - body->base64_size;
          size_t left = body->suffix_size - position;
          size_t take = left < capacity - copied ? left : capacity - copied;

          memcpy(out + copied, body->suffix + position, take);
          body->sent += take;
          copied += take;
        }
    }

  return copied;
}
