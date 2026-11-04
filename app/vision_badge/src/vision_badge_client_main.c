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
  printf("Usage: %s <status|run|provision|audio-loopback> [args]\n", program);
  printf("  audio-loopback <1..5> [0..100]  record then play; default 30%%\n");
  printf("  audio-tone [1..55]  one-second 1000Hz speaker test; default 30%%\n");
  printf("  audio-diag <dac|mic|mem>  read diagnostic counters; no audio\n");
  printf("  audio-diag volume [up|down|1..10|reset]  product output level; no sound\n");
  printf("  audio-cache capture <1..10>  retain one raw PCM in RAM; no playback\n");
  printf("  audio-cache play <0..100> [stream|buffer|normalize]  replay without recording\n");
  printf("  audio-cache <status|clear>  inspect/erase retained PCM; no audio\n");
  printf("  agora wake-test-start  one local detection; never start voice\n");
  printf("  codec-selftest  synthetic Opus test; no recording or playback\n");
  printf("  agora <version|status|stats|link|netstats|rxstats|controlstats|init-test|probe>  SDK / link diagnostics\n");
  printf("  agora-config <JSON>  save LAN voice controller configuration on AP\n");
  printf("  conversation <start|stop|status|probe>  background voice; probe has no audio\n");
#ifdef CONFIG_CONTEST2026_441_POWER
  printf("  Keys: left/right short=volume down/up (1..10); middle short=voice start/stop\n");
  printf("  Middle held 3s=power toggle; release after hold does not start voice\n");
#else
  printf("  Keys: left short=scene; middle short=voice start/stop; holds >=3s ignored\n");
#endif
  printf("  status          check the CP-to-AP vision endpoint\n");
#ifdef CONFIG_CONTEST2026_441_POWER
  printf("  power <status|quiet|resume|prepare|off|toggle>  explicit product power; off requests reboot-to-sleep\n");
#endif
  printf("  battery         read voltage/charge state on AP; no audio\n");
  printf("  camera-test     capture one JPEG; no upload or sound\n");
  printf("  camera <start|stop|status>  warm camera control; no upload or sound\n");
  printf("  camera-compare  first/1s-later JPEGs; use host --capture-dir\n");
  printf("  scene           one JPEG -> text cues; NOT navigation\n");
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

#ifdef CONFIG_CONTEST2026_441_POWER
  if (argc == 3 && !strcmp(argv[1], "power"))
    {
      memset(&result, 0, sizeof(result));
      ret = vision_badge_rpc_power(argv[2], &result);
      printf("vision_badge: %s (transport=%d status=%d)\n", result.text, ret, result.status);
      return ret < 0 || result.status < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
#endif

  if (argc == 2 && strcmp(argv[1], "provision") == 0)
    {
      return vision_badge_provision();
    }

  if (argc == 2 && strcmp(argv[1], "battery") == 0)
    {
      memset(&result, 0, sizeof(result));
      ret = vision_badge_rpc_battery(&result);
      if (ret < 0)
        {
          fprintf(stderr, "vision_badge: battery RPC failed=%d\n", ret);
          return EXIT_FAILURE;
        }

      printf("vision_badge: %s (ret=%d)\n", result.text, result.status);
      return result.status < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (argc == 3 && (strcmp(argv[1], "agora") == 0 || strcmp(argv[1], "agora-config") == 0))
    {
      memset(&result, 0, sizeof(result));
      char action[VISION_BADGE_RPC_TEXT_SIZE];
      int count = snprintf(action, sizeof(action), "%s%s",
                           strcmp(argv[1], "agora-config") == 0 ? "config " : "", argv[2]);
      if (count < 0 || (size_t)count >= sizeof(action)) return EXIT_FAILURE;
      ret = vision_badge_rpc_agora(action, &result);
      explicit_bzero(action, sizeof(action));
      if (ret < 0)
        {
          fprintf(stderr, "vision_badge: Agora RPC failed=%d\n", ret);
          return EXIT_FAILURE;
        }
      printf("vision_badge: %s (ret=%d)\n", result.text, result.status);
      return result.status < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (argc >= 3 && strcmp(argv[1], "conversation") == 0)
    {
      char action[160];
      size_t used = 0;
      int i;
      memset(action, 0, sizeof(action));
      for (i = 2; i < argc; i++)
        {
          int count = snprintf(action + used, sizeof(action) - used,
                                "%s%s", i == 2 ? "" : " ", argv[i]);
          if (count < 0 || (size_t)count >= sizeof(action) - used)
            {
              return EXIT_FAILURE;
            }
          used += count;
        }
      memset(&result, 0, sizeof(result));
      ret = vision_badge_rpc_conversation(action, &result);
      explicit_bzero(action, sizeof(action));
      if (ret < 0)
        {
          fprintf(stderr, "vision_badge: conversation RPC failed=%d\n", ret);
          return EXIT_FAILURE;
        }
      printf("vision_badge: %s (AP status=%d)\n", result.text, result.status);
      return result.status < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (argc == 2 && strcmp(argv[1], "codec-selftest") == 0)
    {
      memset(&result, 0, sizeof(result));
      ret = vision_badge_rpc_codec_selftest(&result);
      if (ret < 0)
        {
          fprintf(stderr, "vision_badge: codec RPC failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      printf("vision_badge: %s (AP status=%d)\n", result.text, result.status);
      return result.status < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if ((argc == 2 || argc == 3) && strcmp(argv[1], "audio-tone") == 0)
    {
      char action[32];
      unsigned long volume = 30;
      char *end;
      if (argc == 3)
        {
          volume = strtoul(argv[2], &end, 10);
          if (argv[2][0] == '\0' || *end != '\0' || volume < 1 || volume > 55)
            return EXIT_FAILURE;
        }
      snprintf(action, sizeof(action), "tone %lu", volume);
      memset(&result, 0, sizeof(result));
      ret = vision_badge_rpc_audio_diagnostic(action, &result);
      if (ret < 0) return EXIT_FAILURE;
      printf("vision_badge: %s (AP status=%d)\n", result.text, result.status);
      return result.status < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (argc >= 3 && strcmp(argv[1], "audio-cache") == 0)
    {
      char action[48];
      char *end;
      unsigned long value;
      if (argc == 3 && (!strcmp(argv[2], "status") || !strcmp(argv[2], "clear")))
        snprintf(action, sizeof(action), "cache %s", argv[2]);
      else if (argc == 4 && !strcmp(argv[2], "capture"))
        {
          value = strtoul(argv[3], &end, 10);
          if (argv[3][0] == '\0' || *end != '\0' || value < 1 || value > 10)
            return EXIT_FAILURE;
          printf("vision_badge: capturing %lu seconds to RAM; no playback\n", value);
          snprintf(action, sizeof(action), "cache capture %lu", value);
        }
      else if ((argc == 4 || argc == 5) && !strcmp(argv[2], "play"))
        {
          const char *path = argc == 5 ? argv[4] : "stream";
          value = strtoul(argv[3], &end, 10);
          if (argv[3][0] == '\0' || *end != '\0' || value > 100 ||
              (strcmp(path, "stream") && strcmp(path, "buffer") && strcmp(path, "normalize")))
            return EXIT_FAILURE;
          snprintf(action, sizeof(action), "cache play %lu %s", value, path);
        }
      else return EXIT_FAILURE;
      memset(&result, 0, sizeof(result));
      ret = vision_badge_rpc_audio_diagnostic(action, &result);
      if (ret < 0)
        {
          fprintf(stderr, "vision_badge: audio cache RPC failed=%d\n", ret);
          return EXIT_FAILURE;
        }
      printf("vision_badge: %s (AP status=%d)\n", result.text, result.status);
      return result.status < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if ((argc == 3 || argc == 4) && strcmp(argv[1], "audio-diag") == 0)
    {
      char action[24];
      if (argc == 4)
        {
          if (strcmp(argv[2], "volume") ||
              (strcmp(argv[3], "up") && strcmp(argv[3], "down") && strcmp(argv[3], "reset") &&
               strcmp(argv[3], "10") && !(strlen(argv[3]) == 1 && argv[3][0] >= '1' && argv[3][0] <= '9')))
            return EXIT_FAILURE;
          snprintf(action, sizeof(action), "volume %s", argv[3]);
        }
      else
        {
          if (strcmp(argv[2], "dac") && strcmp(argv[2], "mic") && strcmp(argv[2], "mem") && strcmp(argv[2], "volume"))
            return EXIT_FAILURE;
          snprintf(action, sizeof(action), "%s", argv[2]);
        }
      memset(&result, 0, sizeof(result));
      ret = vision_badge_rpc_audio_diagnostic(action, &result);
      if (ret < 0) return EXIT_FAILURE;
      printf("vision_badge: %s (AP status=%d)\n", result.text, result.status);
      return result.status < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if ((argc == 3 || argc == 4) &&
      strcmp(argv[1], "audio-loopback") == 0)
    {
      char *end;
      unsigned long seconds = strtoul(argv[2], &end, 10);
      unsigned long volume = 30;

      if (argv[2][0] == '\0' || *end != '\0' || seconds < 1 || seconds > 5)
        {
          return EXIT_FAILURE;
        }

      if (argc == 4)
        {
          volume = strtoul(argv[3], &end, 10);
          if (argv[3][0] == '\0' || *end != '\0' || volume > 100)
            {
              return EXIT_FAILURE;
            }
        }

      printf("vision_badge: AP records %lu seconds, then plays at %lu%%\n",
             seconds, volume);
      memset(&result, 0, sizeof(result));
      ret = vision_badge_rpc_audio_loopback(seconds, volume, &result);
      if (ret < 0)
        {
          fprintf(stderr, "vision_badge: audio RPC failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      printf("vision_badge: %s (AP status=%d)\n", result.text, result.status);
      return result.status < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (argc == 3 && !strcmp(argv[1], "camera") &&
      (!strcmp(argv[2], "start") || !strcmp(argv[2], "stop") || !strcmp(argv[2], "status")))
    {
      memset(&result, 0, sizeof(result));
      ret = vision_badge_rpc_camera(argv[2], &result);
      if (ret < 0) { fprintf(stderr, "vision_badge: camera RPC failed=%d\n", ret); return EXIT_FAILURE; }
      printf("vision_badge: %s (ret=%d)\n", result.text, result.status);
      return result.status < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (argc == 2 && (!strcmp(argv[1], "camera-test") ||
                   !strcmp(argv[1], "camera-compare") || !strcmp(argv[1], "scene")))
    {
      memset(&result, 0, sizeof(result));
      ret = vision_badge_rpc_camera(!strcmp(argv[1], "scene") ? "scene" :
                                   !strcmp(argv[1], "camera-compare") ? "compare" : "capture", &result);
      if (ret < 0) { fprintf(stderr, "vision_badge: camera RPC failed=%d\n", ret); return EXIT_FAILURE; }
      if (result.status < 0)
        { fprintf(stderr, "vision_badge: camera stage=%d failed=%d\n", result.stage, result.status); return EXIT_FAILURE; }
      printf("vision_badge: %s\n", result.text);
      if (!strcmp(argv[1], "scene")) printf("vision_badge: scene cues only; not safe navigation\n");
      return EXIT_SUCCESS;
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
