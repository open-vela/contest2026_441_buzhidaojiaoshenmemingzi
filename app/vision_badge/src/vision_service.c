/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <vision_badge/mimo_http.h>
#include <vision_badge/mimo_request.h>
#include <vision_badge/services.h>

#ifdef CONFIG_CRYPTO_MBEDTLS
#include <netutils/webclient.h>

#define VISION_API_KEY_SIZE 513

/* ========================================================================
 * vision_http_body_callback —— HTTP 请求体回调（流式发送 JPEG）
 *
 * webclient 在发送 POST 请求体时，会多次调用此回调来获取数据。
 * 每次回调时，body 参数指向 vision_mimo_body_s 结构体，
 * 内部维护了 JSON 请求体的当前发送位置。
 *
 * 为什么要用流式发送？
 *   因为 JPEG 图片经过 Base64 编码后会膨胀约 33%，
 *   640×480 的 JPEG 编码后约 33KB，加上 JSON 模板总共约 35KB。
 *   如果一次性全部加载到内存，会占用宝贵的 PSRAM。
 *   流式发送可以分块读取，降低峰值内存占用。
 * ======================================================================== */
static int vision_http_body_callback(void *buffer, size_t *size,
                                     const void **data, size_t requested,
                                     void *arg)
{
  struct vision_mimo_body_s *body = arg;
  size_t capacity = *size < requested ? *size : requested;
  size_t copied;

  copied = vision_mimo_body_read(body, buffer, capacity);
  *data = buffer;
  *size = copied;
  return copied == 0 && body->sent < body->total_size ? -EIO : 0;
}

/* ========================================================================
 * vision_service_network_check —— 网络健康检查（时钟同步 + TLS 验证）
 *
 * 用于验证设备能否建立可信的 HTTPS 连接。
 * 内部会自动完成：NTP 时钟同步 → HTTPS 校时后备 → 严格 TLS 握手。
 * 如果返回 0，说明网络和证书链都正常。
 * ======================================================================== */
int vision_service_network_check(void)
{
  return mimo_http_network_check();
}

/* ========================================================================
 * vision_service_query —— 发送图片到 MiMo 视觉模型并获取回答
 *
 * 这是摄像头链路的核心网络函数，完整流程：
 *   1. 时钟同步（NTP 或 HTTPS 校时后备）
 *   2. 从 SD-NAND 加载 API Key 和 hostname
 *   3. 构建 JSON 请求体（JPEG Base64 + prompt）
 *   4. 通过 TLS 发送 POST 到 /v1/chat/completions
 *   5. 解析 JSON 响应，提取中文回答
 *
 * 请求格式：
 *   POST /v1/chat/completions
 *   Authorization: Bearer <api_key>
 *   Content-Type: application/json
 *   {
 *     "model": "mimo-v2.5",
 *     "messages": [
 *       {"role": "user", "content": [
 *         {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64,..."}},
 *         {"type": "text", "text": "<prompt>"}
 *       ]}
 *     ],
 *     "thinking": {"type": "disabled"}
 *   }
 * ======================================================================== */
int vision_service_query(const struct vision_badge_image_s *image,
                         const char *prompt,
                         struct vision_badge_result_s *result)
{
  struct vision_mimo_body_s body;
  struct mimo_http_sink_s sink;
  char api_key[VISION_API_KEY_SIZE];
  const char *hostname = NULL;
  const char *headers[2];
  char authorization[VISION_API_KEY_SIZE +
                     sizeof("Authorization: Bearer ")];
  int ret;

  if (image == NULL || image->data == NULL || image->size == 0 ||
      image->format != VISION_BADGE_IMAGE_JPEG || prompt == NULL ||
      prompt[0] == '\0' || result == NULL || result->text == NULL ||
      result->capacity < 2)
    {
      return -EINVAL;
    }

  result->length = 0;
  result->text[0] = '\0';
  result->direction_hint = 0;
  memset(api_key, 0, sizeof(api_key));
  memset(authorization, 0, sizeof(authorization));

  /* The AP startup worker normally completes this before the first key press.
   * The same helper is kept here as a serialized fallback for an early query;
   * it reuses the per-boot Wi-Fi result and never starts a second scan.
   */
  ret = mimo_http_prepare_session();
  if (ret < 0 && ret != -ENOSYS)
    {
      syslog(LOG_ERR, "VISION BADGE QUERY stage=network ret=%d\n", ret);
      fprintf(stderr, "vision_service: network preparation failed: %d\n", ret);
      return ret;
    }

  /* ---- 2. 从 SD-NAND 加载 API Key ---- */
  ret = mimo_http_load_credentials(api_key, sizeof(api_key), &hostname);
  if (ret < 0)
    {
      syslog(LOG_ERR, "VISION BADGE QUERY stage=credentials ret=%d\n", ret);
      goto out;
    }

  /* ---- 3. 构建 JSON 请求体（JPEG Base64 + prompt） ---- */
  ret = vision_mimo_body_init(&body, image->data, image->size, prompt);
  if (ret < 0)
    {
      goto out;
    }

  /* 构建 Authorization: Bearer <key> 请求头 */
  ret = snprintf(authorization, sizeof(authorization),
                 "Authorization: Bearer %s", api_key);
  if (ret < 0 || (size_t)ret >= sizeof(authorization))
    {
      ret = -EOVERFLOW;
      goto out_body;
    }

  headers[0] = "Content-Type: application/json";
  headers[1] = authorization;
  sink.result = result;

  printf("vision_badge: sending %lu-byte JPEG to MiMo (%lu-byte request)\n",
         (unsigned long)image->size, (unsigned long)body.total_size);

  /* ---- 4. 通过 TLS 发送 POST 请求 ---- */
  ret = mimo_http_post(hostname, "/v1/chat/completions", api_key,
                       headers, 2, body.total_size,
                       vision_http_body_callback, &body,
                       mimo_http_sink_callback, &sink);
  if (ret < 0)
    {
      syslog(LOG_ERR, "VISION BADGE QUERY stage=http ret=%d\n", ret);
      fprintf(stderr, "vision_badge: MiMo HTTP request failed: %d\n", ret);
      goto out_body;
    }

  if (body.sent != body.total_size)
    {
      ret = -EIO;
      goto out_body;
    }

  printf("vision_badge: MiMo response=%lu bytes\n",
         (unsigned long)result->length);

  /* ---- 5. 解析 JSON 响应，提取中文回答 ---- */
  ret = mimo_http_parse_chat_response(result);
  if (ret < 0)
    {
      syslog(LOG_ERR, "VISION BADGE QUERY stage=response ret=%d\n", ret);
    }
  else
    {
      syslog(LOG_INFO,
             "VISION BADGE QUERY stage=done jpeg=%lu answer=%lu\n",
             (unsigned long)image->size, (unsigned long)result->length);
    }

out_body:
  vision_mimo_body_deinit(&body);
out:
  memset(authorization, 0, sizeof(authorization));
  memset(api_key, 0, sizeof(api_key));
  return ret;
}

#else

int vision_service_network_check(void)
{
  return -ENOSYS;
}

int vision_service_query(const struct vision_badge_image_s *image,
                         const char *prompt,
                         struct vision_badge_result_s *result)
{
  if (image == NULL || image->data == NULL || image->size == 0 ||
      prompt == NULL || prompt[0] == '\0' || result == NULL ||
      result->text == NULL || result->capacity == 0)
    {
      return -EINVAL;
    }

  result->length = 0;
  result->direction_hint = 0;
  return -ENOSYS;
}

#endif
