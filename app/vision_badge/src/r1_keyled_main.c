/* SPDX-License-Identifier: Apache-2.0 */

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <nuttx/board.h>
#include <arch/board/board.h>

#define R1_KEYLED_POLL_US  20000
#define R1_KEYLED_DEFAULT_SECONDS 30
#define R1_KEYLED_MAX_SECONDS 300

static int r1_keyled_duration(int argc, char *argv[])
{
  char *endptr;
  long seconds;

  if (argc == 1)
    {
      return R1_KEYLED_DEFAULT_SECONDS;
    }

  if (argc != 2)
    {
      return -EINVAL;
    }

  errno = 0;
  seconds = strtol(argv[1], &endptr, 10);
  if (errno != 0 || *endptr != '\0' || seconds < 1 ||
      seconds > R1_KEYLED_MAX_SECONDS)
    {
      return -EINVAL;
    }

  return (int)seconds;
}

int main(int argc, char *argv[])
{
  uint32_t previous = UINT32_MAX;
  uint32_t pressed;
  unsigned int iteration;
  unsigned int iterations;
  int seconds;

  seconds = r1_keyled_duration(argc, argv);
  if (seconds < 0)
    {
      fprintf(stderr, "Usage: r1keyled [seconds: 1..300]\n");
      return EXIT_FAILURE;
    }

  board_button_initialize();
  board_userled_initialize();
  board_userled_all(0);

  printf("R1 key/LED test for %d seconds\n", seconds);
  printf("KEY1 -> red, KEY2 -> green, KEY3 -> both\n");

  iterations = (unsigned int)seconds * 1000000u / R1_KEYLED_POLL_US;
  for (iteration = 0; iteration < iterations; iteration++)
    {
      pressed = board_buttons();
      board_userled(BOARD_LED_RED,
                    (pressed & (BUTTON_KEY1_BIT | BUTTON_KEY3_BIT)) != 0);
      board_userled(BOARD_LED_GREEN,
                    (pressed & (BUTTON_KEY2_BIT | BUTTON_KEY3_BIT)) != 0);

      if (pressed != previous)
        {
          printf("keys: KEY1=%u KEY2=%u KEY3=%u mask=0x%02lx\n",
                 (pressed & BUTTON_KEY1_BIT) != 0,
                 (pressed & BUTTON_KEY2_BIT) != 0,
                 (pressed & BUTTON_KEY3_BIT) != 0,
                 (unsigned long)pressed);
          previous = pressed;
        }

      usleep(R1_KEYLED_POLL_US);
    }

  board_userled_all(0);
  printf("R1 key/LED test finished\n");
  return EXIT_SUCCESS;
}
