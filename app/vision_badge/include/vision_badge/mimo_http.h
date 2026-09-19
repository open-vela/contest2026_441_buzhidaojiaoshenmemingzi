/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CONTEST2026_441_VISION_BADGE_MIMO_HTTP_H
#define CONTEST2026_441_VISION_BADGE_MIMO_HTTP_H

#include <stddef.h>
#include <vision_badge/types.h>

#define MIMO_HTTP_API_KEY_SIZE    513
#define MIMO_HTTP_TIMEOUT_SECONDS 120
#define MIMO_HTTP_WORK_SIZE       2048

/* 加载凭据：从 SD-NAND 读取 API Key，根据前缀确定 hostname */
int mimo_http_load_credentials(char *api_key, size_t api_key_size,
                               const char **hostname);

/* Reuse an existing AP link, or connect from the persisted VSWP record. */
int mimo_http_prepare_network(void);

/* Prepare the Wi-Fi link and certificate clock once for the current boot.
 * The operation is serialized so a startup worker and the first user query
 * cannot start two scans or two clock synchronizations concurrently.
 */
int mimo_http_prepare_session(void);

/* Persist Wi-Fi and MiMo credentials in the VSWP record on SD-NAND. */
int mimo_http_save_provision(const char *ssid, const char *password,
                             const char *api_key);

/* 同步系统时钟（NTP + HTTPS 后备） */
int mimo_http_sync_clock(void);

/* 解析 MiMo chat completions 响应中的 choices[0].message.content */
int mimo_http_parse_chat_response(struct vision_badge_result_s *result);

/* 通用 HTTPS POST 到 MiMo API */
int mimo_http_post(const char *hostname, const char *path,
                   const char *api_key,
                   const char *headers[], size_t nheaders,
                   size_t bodylen,
                   int (*body_callback)(void *buffer, size_t *size,
                                        const void **data, size_t requested,
                                        void *arg),
                   void *body_arg,
                   int (*sink_callback)(char **buffer, int offset, int datend,
                                        int *buffer_size, void *arg),
                   void *sink_arg);

/* 网络连通性检查 */
int mimo_http_network_check(void);

/* HTTP 响应 sink 结构体：将响应累积到 vision_badge_result_s.text */
struct mimo_http_sink_s
{
  struct vision_badge_result_s *result;
};

/* 标准 HTTP 响应 sink 回调 */
int mimo_http_sink_callback(char **buffer, int offset, int datend,
                            int *buffer_size, void *arg);

#endif /* CONTEST2026_441_VISION_BADGE_MIMO_HTTP_H */
