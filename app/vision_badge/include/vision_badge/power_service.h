/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VISION_BADGE_POWER_SERVICE_H
#define VISION_BADGE_POWER_SERVICE_H
#include <stdbool.h>
#include <stddef.h>
/* Mutations run on the serialized RPC worker. Reads never start hardware. */
int power_service_command(const char *action, char *text, size_t capacity);
bool power_service_work_allowed(void);
#endif
