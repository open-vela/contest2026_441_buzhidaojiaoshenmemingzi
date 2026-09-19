/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CONTEST2026_441_VISION_BADGE_MIMO_REQUEST_H
#define CONTEST2026_441_VISION_BADGE_MIMO_REQUEST_H

#include <stddef.h>
#include <stdint.h>

struct vision_mimo_body_s
{
  const uint8_t *image;
  size_t image_size;
  char *suffix;
  size_t suffix_size;
  size_t base64_size;
  size_t total_size;
  size_t sent;
};

int vision_mimo_body_init(struct vision_mimo_body_s *body,
                          const uint8_t *image, size_t image_size,
                          const char *prompt);
void vision_mimo_body_deinit(struct vision_mimo_body_s *body);
size_t vision_mimo_body_read(struct vision_mimo_body_s *body,
                             void *buffer, size_t capacity);

#endif /* CONTEST2026_441_VISION_BADGE_MIMO_REQUEST_H */
