/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CONTEST2026_441_VISION_BADGE_SERVICES_H
#define CONTEST2026_441_VISION_BADGE_SERVICES_H

#include <vision_badge/config.h>
#include <vision_badge/types.h>

int camera_service_probe(struct vision_badge_probe_s *probe);
int camera_service_capture(struct vision_badge_image_s *image);
/* R1/AP warm camera control. Capture copies a fresh frame; no auto upload.
 * stop waits for stream/buffer release, start is asynchronous.
 * Internal voice ownership: pause blocks capture/start and remembers wanted
 * state; resume restores it after audio cleanup. No upload or audio here.
 */
int camera_service_control(const char *action, char *text, size_t capacity);
/* Voice coordinator only, AFTER Audio STOP/RELEASE. Leaves pause ownership
 * intact, so ordinary capture/start cannot overlap this cold acquisition.
 * Any failure quarantines the camera until reboot; no stale-frame fallback. */
int camera_service_capture_paused(struct vision_badge_image_s *image);
/* Borrow the warm single owner with camera_service_control("borrow").
 * Copies a post-request fresh frame without stopping Audio or reopening DVP.
 * Caller keeps its buffer until return; timeout/cancellation never use cache.
 * Release the borrow with "resume", restoring the preceding wanted state.
 */
int camera_service_capture_live(struct vision_badge_image_s *image,
                                unsigned int timeout_ms,
                                bool (*cancel)(void *), void *arg);
/* Diagnostic only: first valid frame and a frame >=1s later, same stream.
 * Both caller-owned buffers must remain valid until this returns. No upload.
 */
int camera_service_capture_pair(struct vision_badge_image_s *early,
                                struct vision_badge_image_s *settled);

int audio_service_probe(struct vision_badge_probe_s *probe);
int audio_service_record(void *buffer, size_t capacity, size_t *recorded);
int audio_service_play(const int16_t *pcm, size_t bytes, unsigned int volume);
/* Zero means the original per-session configuration; 1..10 is a product
 * override. Pure RAM read; never performs filesystem or Audio I/O. */
unsigned int audio_service_volume_level(void);
int audio_service_loopback(unsigned int seconds, unsigned int volume,
                           char *summary, size_t capacity);

/* Callback must copy capture samples without network/codec waits. A playback
 * source returns samples, 0 at end, EAGAIN when temporarily buffering. The
 * audio worker owns the device until STOP/RELEASE; cancellation is polled.
 */
typedef bool (*audio_cancel_cb_t)(void *arg);
typedef int (*audio_capture_cb_t)(void *arg, const int16_t *pcm, size_t samples);
typedef int (*audio_source_cb_t)(void *arg, int16_t *pcm, size_t capacity);
int audio_service_capture_stream(audio_capture_cb_t sink,
                                 audio_cancel_cb_t cancel, void *arg);
int audio_service_play_stream(audio_source_cb_t source,
                              audio_cancel_cb_t cancel, void *arg,
                              unsigned int volume);

int vision_service_query(const struct vision_badge_image_s *image,
                         const char *prompt,
                         struct vision_badge_result_s *result);

int feedback_service_present(const struct vision_badge_result_s *result);
int feedback_service_vibrate(int direction_hint);

/* Activity only: enqueue a state for the existing display owner, no I/O here.
 * Disabled candidates compile away so the baseline retains its behavior. */
#ifdef CONFIG_BK7258_AIDK_EYE_LVGL_STATUS
void feedback_service_activity(enum vision_badge_stage_e stage);
#else
static inline void feedback_service_activity(enum vision_badge_stage_e stage)
{
  (void)stage;
}
#endif

#endif /* CONTEST2026_441_VISION_BADGE_SERVICES_H */
