/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-r1/src/bk7258_bringup.c
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
 * 文件角色：板级外设初始化（bk7258_bringup 的实现）
 *
 * bring-up 流程位置：NuttX 内核启动后 → INIT 线程 → board_app_initialize()
 *                    → bk7258_bringup() → 本文件
 *
 * 调用时机：NuttX 内核已经跑起来了，调度器、中断、串口驱动全部就绪，
 * 可以安全地使用 printf、malloc、文件系统等高级功能。
 *
 * 与 arm_boardinitialize() 的区别：
 *   - arm_boardinitialize()：在调度器启动前调用，只能用最基础的操作
 *   - bk7258_bringup()：在调度器启动后调用，可以挂载文件系统、创建设备节点等
 *
 * 调用链：
 *   nx_start() → 创建 INIT 线程 → board_app_initialize() → bk7258_bringup()
 *
 * M1 阶段：只挂载 /proc 文件系统（用于查看系统状态，如 cat /proc/meminfo）
 * 后续阶段可以在这里添加：
 *   - 挂载 /dev 文件系统
 *   - 注册 SPI/I2C 设备
 *   - 初始化 LCD 屏幕
 *   - 初始化音频设备
 *
 * 参考：bk7258_appinit.c（调用 bk7258_bringup）
 *       bk7258-r1.h（声明 bk7258_bringup）
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/fs/fs.h>

#include "bk7258-r1.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_bringup
 *
 * Description:
 *   Minimal R1 bring-up.  Keep this limited to procfs until UART0 and NSH
 *   have been verified on hardware.
 *
 ****************************************************************************/

int bk7258_bringup(void)
{
  int ret = OK;

  /* ── 挂载 /proc 文件系统 ───────────────────────────────────
   * /proc 是一个虚拟文件系统（数据只存在内存里，不写硬盘/Flash）。
   * 挂载后可以用 cat /proc/meminfo 查看内存使用情况，
   * cat /proc/version 查看版本信息，ls /proc/ 列出所有进程等。
   *
   * 通俗理解：/proc 就是系统的"体检报告"，可以随时查看系统状态。
   *
   * nx_mount 参数说明：
   *   NULL        = 源设备（虚拟文件系统不需要设备，所以是 NULL）
   *   "/proc"     = 挂载点（访问路径）
   *   "procfs"    = 文件系统类型（告诉内核用 procfs 驱动）
   *   0           = 挂载标志（0 = 默认选项）
   *   NULL        = 额外数据（不需要额外参数） */

#ifdef CONFIG_FS_PROCFS
  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      /* 挂载失败只是打印警告，不阻止系统继续启动。
       * 因为 /proc 不是关键功能，失败了系统还能正常运行。 */
      syslog(LOG_ERR, "ERROR: Failed to mount procfs at /proc: %d\n", ret);
    }
#endif

  syslog(LOG_INFO, "BK7258 R1 bring-up complete\n");

  UNUSED(ret);       /* 如果 CONFIG_FS_PROCFS 没开，ret 一直等于 OK，消除编译警告 */
  return OK;         /* 始终返回 OK，M1 阶段不因非关键失败而阻止启动 */
}
