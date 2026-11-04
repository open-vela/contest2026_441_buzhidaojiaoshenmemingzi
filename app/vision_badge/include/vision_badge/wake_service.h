/* SPDX-License-Identifier: Apache-2.0 */
#ifndef R1_WAKE_SERVICE_H
#define R1_WAKE_SERVICE_H
#include <stdbool.h>
#include <stddef.h>
#include <vision_badge/config.h>
#ifdef CONFIG_CONTEST2026_441_WAKE
int wake_service_command(const char *action, char *text, size_t capacity);
/* Serialized AP RPC brackets resource-owning commands. A timeout leaves the
 * existing owner intact. End must match Begin, including Begin failures. */
int wake_service_rpc_begin(bool exclusive);
void wake_service_rpc_end(bool exclusive);
#endif
#endif
