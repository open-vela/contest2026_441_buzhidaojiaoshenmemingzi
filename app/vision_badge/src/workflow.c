/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <vision_badge/services.h>
#include <vision_badge/workflow.h>

static int vision_badge_fail(struct vision_badge_context_s *context,
                             int error_code)
{
  context->last_error = error_code;
  return error_code;
}

int vision_badge_run_once(struct vision_badge_context_s *context,
                          const char *prompt,
                          struct vision_badge_image_s *image,
                          struct vision_badge_result_s *result)
{
  int ret;

  if (context == NULL || prompt == NULL || prompt[0] == '\0' ||
      image == NULL || result == NULL)
    {
      return -EINVAL;
    }

  context->stage = VISION_BADGE_STAGE_CAPTURE;
  context->last_error = 0;

  ret = camera_service_capture(image);
  if (ret < 0)
    {
      return vision_badge_fail(context, ret);
    }

  context->stage = VISION_BADGE_STAGE_QUERY;
  ret = vision_service_query(image, prompt, result);
  if (ret < 0)
    {
      return vision_badge_fail(context, ret);
    }

  context->stage = VISION_BADGE_STAGE_FEEDBACK;
  ret = feedback_service_present(result);
  if (ret < 0)
    {
      return vision_badge_fail(context, ret);
    }

  context->stage = VISION_BADGE_STAGE_DONE;
  return 0;
}

const char *vision_badge_stage_name(enum vision_badge_stage_e stage)
{
  switch (stage)
    {
      case VISION_BADGE_STAGE_IDLE:
        return "idle";
      case VISION_BADGE_STAGE_CAPTURE:
        return "capture";
      case VISION_BADGE_STAGE_QUERY:
        return "query";
      case VISION_BADGE_STAGE_FEEDBACK:
        return "feedback";
      case VISION_BADGE_STAGE_DONE:
        return "done";
      case VISION_BADGE_STAGE_ERROR:
        return "error";
      default:
        return "unknown";
    }
}
