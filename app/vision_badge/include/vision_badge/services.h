/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CONTEST2026_441_VISION_BADGE_SERVICES_H
#define CONTEST2026_441_VISION_BADGE_SERVICES_H

#include <vision_badge/types.h>

int camera_service_probe(struct vision_badge_probe_s *probe);
int camera_service_capture(struct vision_badge_image_s *image);

int audio_service_probe(struct vision_badge_probe_s *probe);
int audio_service_record(void *buffer, size_t capacity, size_t *recorded);

int vision_service_query(const struct vision_badge_image_s *image,
                         const char *prompt,
                         struct vision_badge_result_s *result);

int feedback_service_present(const struct vision_badge_result_s *result);
int feedback_service_vibrate(int direction_hint);

#endif /* CONTEST2026_441_VISION_BADGE_SERVICES_H */
