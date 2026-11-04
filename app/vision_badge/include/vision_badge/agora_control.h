/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VISION_BADGE_AGORA_CONTROL_H
#define VISION_BADGE_AGORA_CONTROL_H
#include <netutils/cJSON.h>
#include <vision_badge/services.h>
/* LAN development control only. Account/provider secrets never reach R1.
 * Caller owns the returned session and preserves it until SDK fini succeeds.
 * Every operation has a bounded socket timeout; stop is also attempted when
 * local SDK teardown fails. lease[65] is not printed in logs.
 */
int agora_control_start(const cJSON *local, cJSON **session, char lease[65]);
int agora_control_keepalive(const cJSON *local, const char *lease);
struct agora_scene_request_s
{
  char id[33];
  uint32_t remaining_ms;
};
/* Experimental capability: empty id means no command. Never retries a frame. */
int agora_control_poll(const cJSON *local, const char *lease,
                       struct agora_scene_request_s *request);
int agora_control_scene_request(const cJSON *local,
                                const struct agora_scene_request_s *request,
                                const struct vision_badge_image_s *image);
/* Report a local capture failure once, before any upload was attempted.
 * Uses the same correlated LAN frame endpoint; never sends audio or a JPEG. */
int agora_control_scene_failed(const cJSON *local,
                               const struct agora_scene_request_s *request);
int agora_control_stop(const cJSON *local, const char *lease);
int agora_control_save(const char *json);
/* Separate, non-secret volume record on the shared verified FAT mount.
 * Same temp/flush/fsync/close/rename pattern as the existing config writer.
 * Serialized product RPC owner only; never called by Audio callbacks. */
int agora_control_volume_load(unsigned int *level);
int agora_control_volume_save(unsigned int level);
/* Mount the existing SD-NAND filesystem before config access. Never format. */
int agora_control_prepare_storage(void);
/* Reuse the pinned LAN-only HTTP/auth/redirect/response bounds for optional
 * streaming wake. No cloud account credentials or RTC operations here. */
int agora_control_load(cJSON **local);
struct sockaddr_in;
/* Same validated private endpoint and device key as the HTTP control path. */
int agora_control_wake_udp_target(const cJSON *local, struct sockaddr_in *peer,
                                   unsigned char key[32]);
struct webclient_conn_s;
/* Existing NuttX CONNECT API; one private, authenticated duplex audio socket.
 * Caller owns close/free of the detached connection. No forwarding proxy. */
int agora_control_wake_stream(const cJSON *local, struct webclient_conn_s **conn);
int agora_control_wake(const cJSON *local, const char *operation,
                        const void *body, size_t bytes, const char *sequence,
                        cJSON **reply);
/* Explicit single-frame observation; no RTC, MIC, TTS or navigation. */
int agora_control_scene(const struct vision_badge_image_s *image,
                         char *text, size_t capacity);
int agora_control_capture(const struct vision_badge_image_s *image,
                           char *text, size_t capacity);
#endif
