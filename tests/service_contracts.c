/* SPDX-License-Identifier: Apache-2.0 */

/* Host-only contract checks. No device access or model requests. */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <vision_badge/services.h>
#include <vision_badge/workflow.h>

int main(void)
{
  unsigned char pixels[32] = {0};
  char text[32] = {0};
  struct vision_badge_image_s image = {0};
  struct vision_badge_result_s result = {0};
  struct vision_badge_context_s context = {VISION_BADGE_STAGE_IDLE, 0};
  size_t recorded = 123;

  assert(camera_service_probe(NULL) == -EINVAL);
  assert(audio_service_probe(NULL) == -EINVAL);
  assert(camera_service_capture(NULL) == -EINVAL);
  assert(camera_service_capture(&image) == -EINVAL);
  assert(audio_service_record(NULL, 0, &recorded) == -EINVAL);
  assert(audio_service_record(pixels, sizeof(pixels), NULL) == -EINVAL);
  assert(feedback_service_present(NULL) == -EINVAL);
  assert(feedback_service_present(&result) == -EINVAL);
  assert(feedback_service_vibrate(0) == -ENOSYS);

  image.data = pixels;
  image.capacity = sizeof(pixels);
  image.size = 1;
  assert(camera_service_capture(&image) == -ENOSYS);
  assert(image.size == 0);

  assert(audio_service_record(pixels, sizeof(pixels), &recorded) == -ENOSYS);
  assert(recorded == 0);

  result.text = text;
  result.capacity = sizeof(text);
  assert(vision_service_query(&image, "test", &result) == -EINVAL);
  image.size = 1;
  assert(vision_service_query(&image, "", &result) == -EINVAL);
  result.length = 1;
  result.direction_hint = 1;
  assert(vision_service_query(&image, "test", &result) == -ENOSYS);
  assert(result.length == 0);
  assert(result.direction_hint == 0);

  assert(vision_badge_run_once(NULL, "test", &image, &result) == -EINVAL);
  assert(vision_badge_run_once(&context, "", &image, &result) == -EINVAL);
  assert(context.stage == VISION_BADGE_STAGE_IDLE);
  assert(vision_badge_run_once(&context, "test", &image, &result) == -ENOSYS);
  assert(context.stage == VISION_BADGE_STAGE_CAPTURE);
  assert(context.last_error == -ENOSYS);
  assert(image.size == 0);

  puts("vision_badge: host service contracts passed (stub backends)");
  return 0;
}
