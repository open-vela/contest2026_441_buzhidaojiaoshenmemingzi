/****************************************************************************
 * arch/arm/src/bk7258/bk7258_gpio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "arm_internal.h"
#include "bk7258_gpio.h"
#include "hardware/bk7258_gpio.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int bk7258_gpio_configinput(uint32_t pin, enum bk7258_gpio_pull_e pull)
{
  uint32_t clearbits;
  uint32_t setbits;

  if (pin >= BK7258_GPIO_NPINS || pull > BK7258_GPIO_PULLUP)
    {
      return -EINVAL;
    }

  /* Select ordinary GPIO, disable the output driver and enable input. */

  clearbits = BK7258_GPIO_INPUT_ENABLE |
              BK7258_GPIO_OUTPUT_DISABLE |
              BK7258_GPIO_PULL_UP |
              BK7258_GPIO_PULL_ENABLE |
              BK7258_GPIO_SECOND_FUNCTION;
  setbits = BK7258_GPIO_INPUT_ENABLE | BK7258_GPIO_OUTPUT_DISABLE;

  if (pull != BK7258_GPIO_FLOAT)
    {
      setbits |= BK7258_GPIO_PULL_ENABLE;

      if (pull == BK7258_GPIO_PULLUP)
        {
          setbits |= BK7258_GPIO_PULL_UP;
        }
    }

  modifyreg32(BK7258_GPIO_CFG(pin), clearbits, setbits);
  return OK;
}

int bk7258_gpio_configoutput(uint32_t pin, bool value)
{
  uint32_t clearbits;

  if (pin >= BK7258_GPIO_NPINS)
    {
      return -EINVAL;
    }

  /* Load the first value before enabling the output driver, avoiding a short
   * unwanted pulse on the pin during configuration.
   */

  bk7258_gpio_write(pin, value);

  clearbits = BK7258_GPIO_INPUT_ENABLE |
              BK7258_GPIO_OUTPUT_DISABLE |
              BK7258_GPIO_PULL_UP |
              BK7258_GPIO_PULL_ENABLE |
              BK7258_GPIO_SECOND_FUNCTION;
  modifyreg32(BK7258_GPIO_CFG(pin), clearbits, 0);
  return OK;
}

bool bk7258_gpio_read(uint32_t pin)
{
  DEBUGASSERT(pin < BK7258_GPIO_NPINS);
  return (getreg32(BK7258_GPIO_CFG(pin)) & BK7258_GPIO_INPUT) != 0;
}

void bk7258_gpio_write(uint32_t pin, bool value)
{
  DEBUGASSERT(pin < BK7258_GPIO_NPINS);

  if (value)
    {
      modifyreg32(BK7258_GPIO_CFG(pin), 0, BK7258_GPIO_OUTPUT);
    }
  else
    {
      modifyreg32(BK7258_GPIO_CFG(pin), BK7258_GPIO_OUTPUT, 0);
    }
}
