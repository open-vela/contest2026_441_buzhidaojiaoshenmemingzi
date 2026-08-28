/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-r1/src/bk7258_userleds.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/board.h>
#include <arch/board/board.h>

#include "bk7258-r1.h"
#include "bk7258_gpio.h"

#if defined(CONFIG_ARCH_HAVE_LEDS) && !defined(CONFIG_ARCH_LEDS)

static const uint32_t g_leds[BOARD_NLEDS] =
{
  GPIO_LED_RED,
  GPIO_LED_GREEN
};

uint32_t board_userled_initialize(void)
{
  unsigned int i;

  for (i = 0; i < BOARD_NLEDS; i++)
    {
      bk7258_gpio_configoutput(g_leds[i], false);
    }

  return BOARD_NLEDS;
}

void board_userled(int led, bool ledon)
{
  if ((unsigned int)led < BOARD_NLEDS)
    {
      bk7258_gpio_write(g_leds[led], ledon);
    }
}

void board_userled_all(uint32_t ledset)
{
  board_userled(BOARD_LED_RED, (ledset & BOARD_LED_RED_BIT) != 0);
  board_userled(BOARD_LED_GREEN, (ledset & BOARD_LED_GREEN_BIT) != 0);
}

#endif /* CONFIG_ARCH_HAVE_LEDS && !CONFIG_ARCH_LEDS */
