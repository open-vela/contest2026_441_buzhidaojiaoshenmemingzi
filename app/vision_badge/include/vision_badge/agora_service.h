/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VISION_BADGE_AGORA_SERVICE_H
#define VISION_BADGE_AGORA_SERVICE_H
#include <stddef.h>
#include <stdbool.h>
#include <vision_badge/config.h>
/* start/stop/toggle/probe/init-test enqueue work; status reports completion.
 * Probe joins briefly without audio. Failed SDK cleanup blocks restart.
 */
int agora_service_command(const char *action, char *text, size_t capacity);
#ifdef CONFIG_CONTEST2026_441_WAKE
int agora_service_wake_acquire(void);
bool agora_service_wake_cancelled(void);
void agora_service_wake_release(int device_error);
#endif
#ifdef CONFIG_CONTEST2026_441_POWER
/* Inhibit new sessions and request async stop. 0 means every local SDK/audio
 * owner has exited; EAGAIN is still draining, other errors retain ownership. */
int agora_service_power_quiesce(void);
int agora_service_power_resume(void);
#endif
#endif
