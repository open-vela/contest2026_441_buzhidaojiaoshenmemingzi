/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>

#include <vision_badge/services.h>

int feedback_service_present(const struct vision_badge_result_s *result)
{
  if (result == NULL || result->text == NULL || result->length == 0)
    {
      return -EINVAL;
    }

  printf("vision_badge: %.*s\n", (int)result->length, result->text);
  return 0;
}

int feedback_service_vibrate(int direction_hint)
{
  (void)direction_hint;

  /* GPIO/PWM vibration output is implemented after the pin plan is frozen. */

  return -ENOSYS;
}
