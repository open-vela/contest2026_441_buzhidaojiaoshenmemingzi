/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-r1/src/bk7258_buttons.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/board.h>
#include <arch/board/board.h>

#include "bk7258-r1.h"
#include "bk7258_gpio.h"

#ifdef CONFIG_ARCH_BUTTONS

static const uint32_t g_buttons[NUM_BUTTONS] =
{
  GPIO_KEY1,
  GPIO_KEY2,
  GPIO_KEY3
};

uint32_t board_button_initialize(void)
{
  unsigned int i;

  for (i = 0; i < NUM_BUTTONS; i++)
    {
      bk7258_gpio_configinput(g_buttons[i], BK7258_GPIO_PULLUP);
    }

  return NUM_BUTTONS;
}

uint32_t board_buttons(void)
{
  uint32_t pressed = 0;
  unsigned int i;

  for (i = 0; i < NUM_BUTTONS; i++)
    {
      /* R1 keys are active low: a low electrical level means pressed. */

      if (!bk7258_gpio_read(g_buttons[i]))
        {
          pressed |= 1u << i;
        }
    }

  return pressed;
}

#endif /* CONFIG_ARCH_BUTTONS */
