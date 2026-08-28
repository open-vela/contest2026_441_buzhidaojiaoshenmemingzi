/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-r1/src/bk7258-r1.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/
/****************************************************************************
 * 文件角色：R1 板级内部头文件
 *
 * 声明 bk7258_bringup() 函数，被本目录下的 bk7258_boardinitialize.c、
 * bk7258_appinit.c 等板级源码引用。
 *
 * 与 include/board.h 的区别：
 *   - include/board.h：对外接口，被芯片层（nuttx/arch/arm/src/bk7258/）引用
 *   - bk7258-r1.h：对内接口，只被板级自身代码（src/ 目录下）引用
 ****************************************************************************/

#ifndef __BOARDS_BK7258_R1_SRC_BK7258_R1_H
#define __BOARDS_BK7258_R1_SRC_BK7258_R1_H

#include <nuttx/config.h>

/* Physical R1 pin mapping.  Keep these numbers in the board layer: the
 * BK7258 chip driver knows how to operate a pin, but it must not know which
 * pin the R1 schematic calls KEY1 or LED_RED.
 */

#define GPIO_KEY1       12
#define GPIO_KEY2       13
#define GPIO_KEY3        8
#define GPIO_LED_RED     40
#define GPIO_LED_GREEN   41

int bk7258_bringup(void);

#endif /* __BOARDS_BK7258_R1_SRC_BK7258_R1_H */
