/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CONTEST2026_441_VISION_BADGE_RPC_H
#define CONTEST2026_441_VISION_BADGE_RPC_H

#include <stdbool.h>
#include <stddef.h>
#include <vision_badge/config.h>

#define VISION_BADGE_RPC_TEXT_SIZE 256

struct vision_badge_rpc_result_s
{
  int status;
  int stage;
  char text[VISION_BADGE_RPC_TEXT_SIZE];
};

int vision_badge_rpc_initialize(void);
bool vision_badge_rpc_ready(void);

#ifdef CONFIG_BK7258_AP_CORE
#ifdef CONFIG_CONTEST2026_441_POWER
bool vision_badge_power_network_idle(void);
#endif
/* Serialized AP diagnostics. Only explicit tone/cache-play actions emit sound;
 * cache-capture records only. Status/clear never start either audio path. */
int vision_badge_audio_diagnostic(const char *action, char *text, size_t capacity);
#else
#ifdef CONFIG_CONTEST2026_441_POWER
int vision_badge_rpc_power(const char *action,
                           struct vision_badge_rpc_result_s *result);
#endif
int vision_badge_rpc_camera(const char *action,
                            struct vision_badge_rpc_result_s *result);
int vision_badge_rpc_audio_diagnostic(const char *action,
                                      struct vision_badge_rpc_result_s *result);
int vision_badge_rpc_battery(struct vision_badge_rpc_result_s *result);
int vision_badge_rpc_agora(const char *action,
                           struct vision_badge_rpc_result_s *result);
int vision_badge_rpc_conversation(const char *action,
                                  struct vision_badge_rpc_result_s *result);
int vision_badge_rpc_codec_selftest(struct vision_badge_rpc_result_s *result);
int vision_badge_rpc_audio_loopback(unsigned int seconds,
                                    unsigned int volume,
                                    struct vision_badge_rpc_result_s *result);
int vision_badge_rpc_query(const char *prompt, unsigned int timeout_ms,
                           struct vision_badge_rpc_result_s *result);
int vision_badge_rpc_provision(const char *ssid, const char *password,
                               const char *api_key, unsigned int timeout_ms,
                               struct vision_badge_rpc_result_s *result);
#endif

#endif /* CONTEST2026_441_VISION_BADGE_RPC_H */
