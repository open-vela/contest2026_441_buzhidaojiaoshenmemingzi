/****************************************************************************
 * arch/arm/src/bk7258/chip.h
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/
/****************************************************************************
 * 文件角色：芯片层主头文件（汇总头文件）
 *
 * 这是 NuttX 芯片层编译的入口头文件，所有芯片层 .c 文件都会 #include "chip.h"。
 * 它做了两件事：
 *   1. 引入硬件寄存器地址（hardware/bk7258_memorymap.h）
 *   2. 告诉 NuttX 内核这个芯片有多少个外设中断（ARMV8M_PERIPHERAL_INTERRUPTS）
 *
 * 这个值决定了向量表 _vectors[] 的大小——向量表需要为每个中断预留一个位置。
 * BK7258 有 64 个外设中断（中断号 16-79），所以向量表大小 = 16(系统异常) + 64 = 80。
 *
 * 中断编号定义在 arch/arm/include/bk7258/irq.h 中。
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_BK7258_CHIP_H
#define __ARCH_ARM_SRC_BK7258_CHIP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/* Include the memory map and chip peripheral definitions.  The NVIC
 * priority macros come from <arch/chip/chip.h>.
 */

#include "hardware/bk7258_memorymap.h"

/* Number of external (peripheral) interrupts used by the armv8-m vector
 * table.  BK7258_IRQ_NEXTINT is defined in arch/arm/include/bk7258/irq.h
 * (pulled in via nuttx/irq.h).
 */

#define ARMV8M_PERIPHERAL_INTERRUPTS BK7258_IRQ_NEXTINT

#endif /* __ARCH_ARM_SRC_BK7258_CHIP_H */
