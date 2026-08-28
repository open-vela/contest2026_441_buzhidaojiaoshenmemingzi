/****************************************************************************
 * arch/arm/src/bk7258/bk7258_gpio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_BK7258_BK7258_GPIO_H
#define __ARCH_ARM_SRC_BK7258_BK7258_GPIO_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

enum bk7258_gpio_pull_e
{
  BK7258_GPIO_FLOAT = 0,
  BK7258_GPIO_PULLDOWN,
  BK7258_GPIO_PULLUP
};

int  bk7258_gpio_configinput(uint32_t pin, enum bk7258_gpio_pull_e pull);
int  bk7258_gpio_configoutput(uint32_t pin, bool value);
bool bk7258_gpio_read(uint32_t pin);
void bk7258_gpio_write(uint32_t pin, bool value);

#endif /* __ARCH_ARM_SRC_BK7258_BK7258_GPIO_H */
