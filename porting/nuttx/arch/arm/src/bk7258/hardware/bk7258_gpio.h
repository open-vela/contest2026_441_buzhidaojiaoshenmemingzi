/****************************************************************************
 * arch/arm/src/bk7258/hardware/bk7258_gpio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_BK7258_HARDWARE_BK7258_GPIO_H
#define __ARCH_ARM_SRC_BK7258_HARDWARE_BK7258_GPIO_H

#include <nuttx/config.h>

#include "bk7258_memorymap.h"

/* BK7258 has GPIO0 through GPIO55.  Each pin owns one 32-bit configuration
 * register in the always-on GPIO block.
 */

#define BK7258_GPIO_NPINS              56
#define BK7258_GPIO_CFG(n)             (BK7258_AON_GPIO_BASE + ((n) << 2))

/* GPIO_CFG register fields, confirmed against the BK7258 AIDK gpio_struct.h
 * and gpio_ll.h.  GPIO_OUTPUT_DISABLE is active high: clearing the bit enables
 * the output driver.
 */

#define BK7258_GPIO_INPUT              (1u << 0)
#define BK7258_GPIO_OUTPUT             (1u << 1)
#define BK7258_GPIO_INPUT_ENABLE       (1u << 2)
#define BK7258_GPIO_OUTPUT_DISABLE     (1u << 3)
#define BK7258_GPIO_PULL_UP            (1u << 4)
#define BK7258_GPIO_PULL_ENABLE        (1u << 5)
#define BK7258_GPIO_SECOND_FUNCTION    (1u << 6)

#endif /* __ARCH_ARM_SRC_BK7258_HARDWARE_BK7258_GPIO_H */
