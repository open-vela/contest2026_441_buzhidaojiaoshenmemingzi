/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>

#include <vision_badge/services.h>

#ifdef CONFIG_BK7258_AIDK_EYE_LVGL_STATUS
#include <arch/board/board.h>

void feedback_service_activity(enum vision_badge_stage_e stage)
{
  enum bk7258_aidk_eye_state_e state;
  switch (stage)
    {
      case VISION_BADGE_STAGE_IDLE: state = BK7258_AIDK_EYE_IDLE; break;
      case VISION_BADGE_STAGE_CAPTURE: state = BK7258_AIDK_EYE_CAPTURE; break;
      case VISION_BADGE_STAGE_QUERY:
      case VISION_BADGE_STAGE_FEEDBACK: state = BK7258_AIDK_EYE_QUERY; break;
      case VISION_BADGE_STAGE_DONE: state = BK7258_AIDK_EYE_DONE; break;
      case VISION_BADGE_STAGE_ERROR: state = BK7258_AIDK_EYE_ERROR; break;
      default: return;
    }
  /* The worker renders and writes the panels. Failure to enqueue feedback
   * never changes capture/voice ownership or masks a workflow error. */
  (void)bk7258_aidk_eye_set_state(state);
}
#endif

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
