/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>

#include <vision_badge/services.h>
#include <vision_badge/workflow.h>

static int vision_badge_fail(struct vision_badge_context_s *context,
                             int error_code)
{
  context->last_error = error_code;
  context->stage = VISION_BADGE_STAGE_ERROR;
  (void)feedback_service_stage(VISION_BADGE_STAGE_ERROR);
  return error_code;
}

static void vision_badge_set_stage(struct vision_badge_context_s *context,
                                   enum vision_badge_stage_e stage)
{
  context->stage = stage;
  (void)feedback_service_stage(stage);
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

  vision_badge_set_stage(context, VISION_BADGE_STAGE_CAPTURE);
  context->last_error = 0;

  printf("vision_badge: taking a picture\n");
  fflush(stdout);
  ret = camera_service_capture(image);
  if (ret < 0)
    {
      return vision_badge_fail(context, ret);
    }

  printf("vision_badge: picture ready, %ux%u, %u bytes\n",
         image->width, image->height, (unsigned int)image->size);
  vision_badge_set_stage(context, VISION_BADGE_STAGE_QUERY);
  printf("vision_badge: sending the picture to MiMo\n");
  fflush(stdout);
  ret = vision_service_query(image, prompt, result);
  if (ret < 0)
    {
      return vision_badge_fail(context, ret);
    }

  vision_badge_set_stage(context, VISION_BADGE_STAGE_FEEDBACK);
  printf("vision_badge: answer received\n");
  ret = feedback_service_present(result);
  if (ret < 0)
    {
      return vision_badge_fail(context, ret);
    }

  vision_badge_set_stage(context, VISION_BADGE_STAGE_DONE);
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
