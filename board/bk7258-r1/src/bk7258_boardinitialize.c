/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-r1/src/bk7258_boardinitialize.c
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
 * 文件角色：板级早期初始化（arm_boardinitialize 的实现）
 *
 * bring-up 流程位置：bk7258_cstart() → arm_boardinitialize() → 本文件
 *
 * 调用时机：在 NuttX 调度器启动之前被调用，用于做一些"必须在操作系统跑起来
 * 之前完成"的板级初始化，比如：
 *   - GPIO 方向配置（输入/输出）
 *   - 外设复位引脚拉高
 *   - LCD 背光使能
 *   - 特殊电源管理配置
 *
 * M1 阶段：保持空函数，因为 R1 板子还没有需要早期初始化的外设。
 * UART0 的初始化已经在芯片层（bk7258_lowputc.c）完成。
 *
 * 后续阶段可以在这里添加：
 *   - GPIO 初始化（LED、按键等）
 *   - 外设复位引脚控制
 *   - 时钟配置
 *
 * 参考：include/board.h（声明 arm_boardinitialize）
 *       nuttx/arch/arm/src/bk7258/bk7258_start.c（调用 arm_boardinitialize）
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/board.h>

#include "arm_internal.h"
#include "bk7258-r1.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm_boardinitialize
 *
 * Description:
 *   Called early by __start (bk7258_start.c) for low-level board init.
 *   The first R1 milestone intentionally leaves nonessential peripherals
 *   disabled.  The chip layer configures the UART0 download/log pins.
 *
 ****************************************************************************/

void arm_boardinitialize(void)
{
  /* ── M1 阶段：空函数 ───────────────────────────────────────
   * 调度器启动前（此阶段）可以做的事情：
   *   - GPIO 初始化（设置方向、默认电平）
   *   - 外设复位引脚（拉高/拉低使能）
   *   - LCD 背光使能（让屏幕先亮起来）
   *   - 特殊电源管理配置
   *
   * 为什么现在是空的？
   *   R1 板子还没有需要早期初始化的外设。UART0 的初始化
   *   已经在芯片层（bk7258_lowputc.c）完成了。
   *
   * 后续阶段添加示例：
   *   bk7258_gpio_config(GPIO_LED, GPIO_OUTPUT | GPIO_VALUE_ONE);
   *   bk7258_gpio_config(GPIO_BTN, GPIO_INPUT | GPIO_PULLUP);
   *
   * 参考：include/board.h（定义 GPIO 引脚编号）
   *       nuttx/arch/arm/src/bk7258/bk7258_start.c（调用 arm_boardinitialize） */
}
