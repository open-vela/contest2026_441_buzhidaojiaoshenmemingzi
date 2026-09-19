/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>

#include <vision_badge/services.h>

#if defined(CONFIG_BK7258_AP_CORE) && defined(CONFIG_BK7258_AIDK_DUAL_LCD)
#  include <arch/board/board.h>
#endif

int feedback_service_stage(enum vision_badge_stage_e stage)
{
#if defined(CONFIG_BK7258_AP_CORE) && defined(CONFIG_BK7258_AIDK_DUAL_LCD)
  enum bk7258_aidk_eye_state_e eye_state;

  switch (stage)
    {
      case VISION_BADGE_STAGE_CAPTURE:
        eye_state = BK7258_AIDK_EYE_CAPTURE;
        break;
      case VISION_BADGE_STAGE_QUERY:
        eye_state = BK7258_AIDK_EYE_QUERY;
        break;
      case VISION_BADGE_STAGE_FEEDBACK:
      case VISION_BADGE_STAGE_DONE:
        eye_state = BK7258_AIDK_EYE_DONE;
        break;
      case VISION_BADGE_STAGE_ERROR:
        eye_state = BK7258_AIDK_EYE_ERROR;
        break;
      case VISION_BADGE_STAGE_IDLE:
      default:
        eye_state = BK7258_AIDK_EYE_IDLE;
        break;
    }

  (void)bk7258_aidk_eye_set_state(eye_state);
#else
  (void)stage;
#endif
  return 0;
}

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
