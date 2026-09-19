/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CONTEST2026_441_VISION_BADGE_RPC_H
#define CONTEST2026_441_VISION_BADGE_RPC_H

#include <stdbool.h>
#include <stddef.h>

#define VISION_BADGE_RPC_TEXT_SIZE 256

struct vision_badge_rpc_result_s
{
  int status;
  int stage;
  char text[VISION_BADGE_RPC_TEXT_SIZE];
};

int vision_badge_rpc_initialize(void);
bool vision_badge_rpc_ready(void);

#ifndef CONFIG_BK7258_AP_CORE
int vision_badge_rpc_query(const char *prompt, unsigned int timeout_ms,
                           struct vision_badge_rpc_result_s *result);
int vision_badge_rpc_provision(const char *ssid, const char *password,
                               const char *api_key, unsigned int timeout_ms,
                               struct vision_badge_rpc_result_s *result);
#endif

#endif /* CONTEST2026_441_VISION_BADGE_RPC_H */
