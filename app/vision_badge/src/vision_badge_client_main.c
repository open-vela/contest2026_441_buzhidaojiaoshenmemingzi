/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <vision_badge/rpc.h>

#define VISION_BADGE_QUERY_TIMEOUT_MS 90000u

static void vision_badge_client_usage(const char *program)
{
  printf("Usage: %s <status|run|provision> [question]\n", program);
  printf("  status          check the CP-to-AP vision endpoint\n");
  printf("  run <question>  capture one JPEG on AP and query MiMo\n");
  printf("  provision       securely save Wi-Fi and MiMo credentials on AP\n");
}

static int vision_badge_read_hidden(const char *prompt, char *buffer,
                                     size_t capacity, bool allow_empty)
{
  struct termios original;
  struct termios hidden;
  size_t length;
  int ret;

  if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &original) < 0)
    {
      return -ENOTTY;
    }

  hidden = original;
  hidden.c_lflag &= ~(ECHO | ECHONL);
  if (tcsetattr(STDIN_FILENO, TCSANOW, &hidden) < 0)
    {
      return -errno;
    }

  (void)tcflush(STDIN_FILENO, TCIFLUSH);
  printf("%s", prompt);
  fflush(stdout);
  errno = 0;
  if (fgets(buffer, capacity, stdin) == NULL)
    {
      ret = errno != 0 ? -errno : -EIO;
      goto out;
    }

  length = strlen(buffer);
  if (length == capacity - 1 && buffer[length - 1] != '\n' &&
      buffer[length - 1] != '\r')
    {
      ret = -E2BIG;
      goto out;
    }

  while (length > 0 &&
         (buffer[length - 1] == '\n' || buffer[length - 1] == '\r'))
    {
      buffer[--length] = '\0';
    }

  ret = length > 0 || allow_empty ? 0 : -EINVAL;

out:
  (void)tcsetattr(STDIN_FILENO, TCSANOW, &original);
  putchar('\n');
  return ret;
}

static int vision_badge_provision(void)
{
  struct vision_badge_rpc_result_s result;
  char ssid[33] = {0};
  char password[64] = {0};
  char api_key[VISION_BADGE_RPC_TEXT_SIZE] = {0};
  int ret;

  ret = vision_badge_read_hidden("Wi-Fi SSID: ", ssid, sizeof(ssid), false);
  if (ret >= 0)
    {
      ret = vision_badge_read_hidden("Wi-Fi password: ", password,
                                     sizeof(password), true);
    }

  if (ret >= 0)
    {
      ret = vision_badge_read_hidden("MiMo API key: ", api_key,
                                     sizeof(api_key), false);
    }

  if (ret >= 0)
    {
      memset(&result, 0, sizeof(result));
      ret = vision_badge_rpc_provision(ssid, password, api_key,
                                       10000, &result);
      if (ret >= 0)
        {
          ret = result.status;
        }
    }

  explicit_bzero(ssid, sizeof(ssid));
  explicit_bzero(password, sizeof(password));
  explicit_bzero(api_key, sizeof(api_key));

  if (ret < 0)
    {
      fprintf(stderr, "vision_badge: provisioning failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  printf("vision_badge: credentials saved on AP\n");
  return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
  struct vision_badge_rpc_result_s result;
  int ret;

  ret = vision_badge_rpc_initialize();
  if (ret < 0)
    {
      fprintf(stderr, "vision_badge: RPC init failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  if (argc == 2 && strcmp(argv[1], "status") == 0)
    {
      printf("vision_badge: AP endpoint %s\n",
             vision_badge_rpc_ready() ? "ready" : "pending");
      return vision_badge_rpc_ready() ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  if (argc == 2 && strcmp(argv[1], "provision") == 0)
    {
      return vision_badge_provision();
    }

  if (argc == 3 && strcmp(argv[1], "run") == 0)
    {
      memset(&result, 0, sizeof(result));
      ret = vision_badge_rpc_query(argv[2], VISION_BADGE_QUERY_TIMEOUT_MS,
                                   &result);
      if (ret < 0)
        {
          fprintf(stderr, "vision_badge: RPC failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      if (result.status < 0)
        {
          fprintf(stderr, "vision_badge: AP stage=%d error=%d\n",
                  result.stage, result.status);
          return EXIT_FAILURE;
        }

      printf("vision_badge: %s\n", result.text);
      return EXIT_SUCCESS;
    }

  vision_badge_client_usage(argv[0]);
  return EXIT_FAILURE;
}
