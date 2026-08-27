/****************************************************************************
 * arch/arm/src/bk7258/bk7258_lowputc.h
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
 * 文件角色：早期串口输出声明（bk7258_lowsetup 函数声明）
 *
 * 声明 bk7258_lowsetup() 函数，这是 bring-up 流程中第一个被调用的外设初始化函数。
 * 定义在 bk7258_lowputc.c 中，被 bk7258_start.c 的 bk7258_cstart() 调用。
 *
 * 它的作用：使能 UART0 时钟、配置 GPIO 引脚、配好波特率，让芯片能通过串口说话。
 * 没有它，showprogress('A') 就输出不了任何东西，调试完全抓瞎。
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_BK7258_BK7258_LOWPUTC_H
#define __ARCH_ARM_SRC_BK7258_BK7258_LOWPUTC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_lowsetup
 *
 * Description:
 *   Configure the console UART (UART0) to 115200 8N1 and enable TX.
 *   Called once by the chip start-up path (bk7258_start) before
 *   arm_lowputc() becomes usable.
 *
 ****************************************************************************/

void bk7258_lowsetup(void);

#endif /* __ARCH_ARM_SRC_BK7258_BK7258_LOWPUTC_H */
