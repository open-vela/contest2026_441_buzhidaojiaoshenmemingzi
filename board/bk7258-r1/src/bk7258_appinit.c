/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-r1/src/bk7258_appinit.c
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
 * 文件角色：应用初始化入口（NuttX 内核调用板级初始化的入口文件）
 *
 * bring-up 流程位置：nsx_start() → INIT 线程 → board_app_initialize()
 *                    → 本文件 → bk7258_bringup()
 *
 * 通俗理解：内核启动后，会自动创建一个 INIT 线程来初始化板级外设。
 * 这个文件就是 INIT 线程的"第一站"——它调用 bk7258_bringup() 来完成实际的
 * 板级初始化工作（挂载文件系统、注册设备等）。
 *
 * 两个函数的路由逻辑：
 *   - board_app_initialize()：默认走这条路，直接调用 bk7258_bringup()
 *   - board_late_initialize()：如果 CONFIG_BOARD_LATE_INITIALIZE 开启，
 *     则在更晚的阶段调用 bk7258_bringup()
 *
 * 两条路线最终都会到达 bk7258_bringup() → 挂载 /proc 文件系统。
 *
 * 参考：bk7258_bringup.c（实际的板级初始化逻辑）
 *       bk7258-r1.h（声明 bk7258_bringup）
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <syslog.h>

#include <nuttx/board.h>

#include "bk7258-r1.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_app_initialize
 *
 * Description:
 *   Perform application specific initialization.  Called by the system
 *   after the NuttX kernel has been booted and the C libraries are ready.
 *
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{
  /* ── 路由逻辑 ─────────────────────────────────────────────────
   * 两条路，取决于 CONFIG_BOARD_LATE_INITIALIZE 配置：
   *
   * 路 1（默认）：CONFIG_BOARD_LATE_INITIALIZE 没开
   *   → 直接在这里调 bk7258_bringup()，挂载 /proc 文件系统
   *   → 这是 M1 阶段用的路径，简单直接
   *
   * 路 2（可选）：CONFIG_BOARD_LATE_INITIALIZE 开了
   *   → 这里什么都不做，返回 OK
   *   → 内核晚些时候会调 board_late_initialize()，在那里再调 bk7258_bringup()
   *   → 适用于需要在更晚阶段初始化外设的场景（比如依赖其他驱动先加载）
   * ─────────────────────────────────────────────────────────── */

#ifndef CONFIG_BOARD_LATE_INITIALIZE
  return bk7258_bringup();        /* 默认路径：立即初始化板级外设 */
#else
  UNUSED(arg);                    /* arg 参数暂时用不到（M1 阶段无额外参数） */
  return OK;                      /* 推迟到 board_late_initialize() 再初始化 */
#endif
}

#ifdef CONFIG_BOARD_LATE_INITIALIZE

/****************************************************************************
 * Name: board_late_initialize
 *
 * Description:
 *   Called when CONFIG_BOARD_LATE_INITIALIZE is enabled to run board
 *   initialization.
 *
 ****************************************************************************/

void board_late_initialize(void)
{
  /* ── 延迟初始化路径 ─────────────────────────────────────────
   * 只有 CONFIG_BOARD_LATE_INITIALIZE 开了才会编译这个函数。
   *
   * 调用时机：比 board_app_initialize() 更晚，内核调度器已经跑起来了，
   * 可以安全地使用文件系统、设备驱动等高级功能。
   *
   * 当前直接调 bk7258_bringup()，和默认路径做一样的事（挂载 /proc）。
   * 后续阶段可以在这里添加更多依赖其他驱动的初始化（如 LCD、音频等）。 */
  bk7258_bringup();
}
#endif
