/****************************************************************************
 * arch/arm/src/bk7258/bk7258_timerisr.c
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
 * 文件角色：系统时钟心跳（SysTick 定时器）
 *
 * bring-up 流程位置：nx_start() → up_timer_initialize() → 本文件
 *
 * 通俗理解：SysTick 是 Arm 芯片内置的"倒计时闹钟"。每倒数到 0 就响一次
 * （产生中断），然后自动重装，周而复始。NuttX 内核就拿这个闹钟当"心跳"——
 * 每次响就检查一下是不是该切换任务了（时间片轮转）。
 *
 * 参数计算：
 *   SYSTICK_RELOAD = BOARD_SYSTICK_CLOCK / CLK_TCK - 1
 *   当前 R1 配置把 BOARD_SYSTICK_CLOCK 设为 240MHz；如果 CLK_TCK=100，
 *   就是每数 240 万个时钟周期触发一次（每次 10ms）。时钟值尚待实机核对。
 *
 * 调用链：
 *   up_timer_initialize()
 *     ├─ 设置 SysTick 重载值
 *     └─ systick_initialize(true, BOARD_SYSTICK_CLOCK, -1)
 *         true  = 用板级配置给出的 CPU 时钟做时钟源
 *         -1   = 用默认中断优先级
 *
 * 参考：arch/arm/src/mps/mps_timer.c（Armv8-M SysTick 通用模板）
 *       vendor/beken/boards/bk7258/bk7258-r1/include/board.h
 *         → BOARD_SYSTICK_CLOCK（当前候选值 240000000）
 ****************************************************************************/

/* Porting reference: arch/arm/src/mps/mps_timer.c (armv8-m SysTick). */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arch/board/board.h>
#include <nuttx/timers/arch_timer.h>

#include "arm_internal.h"
#include "systick.h"
#include "nvic.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SYSTICK_RELOAD = 时钟频率 / 每秒 tick 数 - 1
 * 例如：240MHz / 100 = 2,400,000 - 1 = 2,399,999
 * 意思是 SysTick 每数 240 万个时钟周期就触发一次中断（每次 10ms）
 * 具体值待实机测量确认。 */

#define SYSTICK_RELOAD ((BOARD_SYSTICK_CLOCK / CLK_TCK) - 1)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* ── up_timer_initialize ───────────────────────────────────────────────────
 * 系统时钟初始化（nx_start() 调用，在中断系统初始化之前）。
 *
 * 为什么要用 SysTick？
 *   SysTick 是 Arm 芯片内置的 24 位倒计时器，所有 Cortex-M 芯片都有。
 *   不需要额外外设，配置简单，NuttX 内核直接用它做任务切换的时钟心跳。
 *
 * 两个步骤：
 *   1. 设置重载值（SYSTICK_RELOAD）：每次倒计时到 0 后自动重新加载这个值
 *   2. 调用 systick_initialize()：启动 SysTick，注册中断处理函数
 *      - true = 用 CPU 时钟（240MHz）做时钟源
 *      - -1  = 用默认中断优先级 */

void up_timer_initialize(void)
{
  putreg32(SYSTICK_RELOAD, NVIC_SYSTICK_RELOAD);  /* 设置重载值 */
  up_timer_set_lowerhalf(systick_initialize(true, BOARD_SYSTICK_CLOCK, -1));
}
