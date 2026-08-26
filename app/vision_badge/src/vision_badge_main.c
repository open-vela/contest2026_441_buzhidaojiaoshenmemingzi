/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vision_badge/config.h>
#include <vision_badge/services.h>
#include <vision_badge/workflow.h>

static void vision_badge_usage(const char *program)
{
  printf("Usage: %s <status|run|selftest> [question]\n", program);
  printf("  status            probe configured device nodes\n");
  printf("  run <question>    execute one capture-query-feedback cycle\n");
  printf("  selftest          validate application contracts without hardware\n");
}

static void vision_badge_print_probe(const char *name, const char *path,
                                     const struct vision_badge_probe_s *probe)
{
  if (probe->available)
    {
      printf("%-8s ready    %s\n", name, path);
    }
  else
    {
      printf("%-8s pending  %s (errno=%d)\n", name, path,
             probe->error_code);
    }
}

static int vision_badge_status(void)
{
  struct vision_badge_probe_s camera = {0};
  struct vision_badge_probe_s audio = {0};

  camera_service_probe(&camera);
  audio_service_probe(&audio);

  printf("vision_badge foundation 0.1.0\n");
  printf("board: BK7258 R1 / openvela\n");
  vision_badge_print_probe("camera", CONFIG_CONTEST2026_441_VISION_BADGE_CAMERA_DEVPATH,
                           &camera);
  vision_badge_print_probe("audio", CONFIG_CONTEST2026_441_VISION_BADGE_AUDIO_DEVPATH,
                           &audio);
  printf("cloud    pending  MiMo adapter is not implemented\n");
  printf("feedback console  enabled; audio/vibration pending\n");
  return 0;
}

static int vision_badge_selftest(void)
{
  struct vision_badge_context_s context = {VISION_BADGE_STAGE_IDLE, 0};
  int ret;

  ret = vision_badge_run_once(&context, NULL, NULL, NULL);
  if (ret != -EINVAL || strcmp(vision_badge_stage_name(context.stage), "idle") != 0)
    {
      fprintf(stderr, "vision_badge: contract selftest failed\n");
      return EXIT_FAILURE;
    }

  printf("vision_badge: contract selftest passed\n");
  return EXIT_SUCCESS;
}

static int vision_badge_run(const char *prompt)
{
  struct vision_badge_context_s context = {VISION_BADGE_STAGE_IDLE, 0};
  struct vision_badge_image_s image = {0};
  struct vision_badge_result_s result = {0};
  int ret;

  image.capacity = CONFIG_CONTEST2026_441_VISION_BADGE_JPEG_BUFFER_SIZE;
  result.capacity = CONFIG_CONTEST2026_441_VISION_BADGE_RESULT_BUFFER_SIZE;
  image.data = malloc(image.capacity);
  result.text = malloc(result.capacity);
  if (image.data == NULL || result.text == NULL)
    {
      fprintf(stderr, "vision_badge: buffer allocation failed\n");
      free(image.data);
      free(result.text);
      return EXIT_FAILURE;
    }

  ret = vision_badge_run_once(&context, prompt, &image, &result);
  if (ret < 0)
    {
      fprintf(stderr, "vision_badge: stage=%s error=%d\n",
              vision_badge_stage_name(context.stage), ret);
    }

  free(image.data);
  free(result.text);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
  if (argc < 2)
    {
      vision_badge_usage(argv[0]);
      return EXIT_FAILURE;
    }

  if (strcmp(argv[1], "status") == 0)
    {
      return vision_badge_status();
    }

  if (strcmp(argv[1], "selftest") == 0)
    {
      return vision_badge_selftest();
    }

  if (strcmp(argv[1], "run") == 0 && argc >= 3)
    {
      return vision_badge_run(argv[2]);
    }

  vision_badge_usage(argv[0]);
  return EXIT_FAILURE;
}
