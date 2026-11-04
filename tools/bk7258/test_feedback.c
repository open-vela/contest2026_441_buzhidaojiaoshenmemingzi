/* SPDX-License-Identifier: Apache-2.0 */
/* Real workflow/feedback adapter; only camera/model/display queue are fakes. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <arch/board/board.h>
#include <vision_badge/services.h>
#include <vision_badge/workflow.h>

static enum bk7258_aidk_eye_state_e events[16];
static unsigned int count;
static int capture_error, query_error, queue_error;

int bk7258_aidk_eye_set_state(enum bk7258_aidk_eye_state_e state)
{
  assert(count < 16);
  events[count++] = state;
  return queue_error;
}

int camera_service_capture(struct vision_badge_image_s *image)
{
  (void)image;
  return capture_error;
}

int vision_service_query(const struct vision_badge_image_s *image,
                         const char *prompt,
                         struct vision_badge_result_s *result)
{
  (void)image;
  (void)prompt;
  result->length = 1;
  return query_error;
}

int main(void)
{
  struct vision_badge_context_s context = {0};
  struct vision_badge_image_s image = {0};
  struct vision_badge_result_s result = {.text = "x"};
  assert(vision_badge_run_once(&context, "", &image, &result) == -EINVAL);
  assert(count == 0);
  for (unsigned int scenario = 0; scenario < 5; scenario++)
    {
      count = 0;
      capture_error = scenario == 1 ? -EIO : 0;
      query_error = scenario == 2 ? -EREMOTEIO : 0;
      result.text = scenario == 3 ? NULL : "x";
      queue_error = scenario == 4 ? -ENODEV : 0;
      int ret = vision_badge_run_once(&context, "fixture", &image, &result);
      assert(ret == (scenario == 1 ? -EIO : scenario == 2 ? -EREMOTEIO :
                     scenario == 3 ? -EINVAL : 0));
      assert(events[0] == BK7258_AIDK_EYE_CAPTURE);
      assert(count == (scenario == 1 ? 2u : 3u));
      if (scenario != 1) assert(events[1] == BK7258_AIDK_EYE_QUERY);
      assert(events[count - 1] == (ret ? BK7258_AIDK_EYE_ERROR : BK7258_AIDK_EYE_DONE));
      assert(context.last_error == ret);
    }
  count = 0;
  feedback_service_activity(VISION_BADGE_STAGE_IDLE);
  feedback_service_activity(VISION_BADGE_STAGE_FEEDBACK);
  feedback_service_activity((enum vision_badge_stage_e)99);
  assert(count == 2 && events[0] == BK7258_AIDK_EYE_IDLE &&
         events[1] == BK7258_AIDK_EYE_QUERY);
  puts("FEEDBACK_WORKFLOW_PASS success/capture/query/feedback/queue errors; no hardware");
  return 0;
}
