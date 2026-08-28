/****************************************************************************
 * arch/arm/src/bk7258/bk7258_allocateheap.c
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
 * 文件角色：堆内存分配（malloc 的底层实现）
 *
 * bring-up 流程位置：nx_start() → up_allocate_heap() → 本文件
 *
 * 通俗理解：你的程序调用 malloc(100) 时，内存从哪来？这里定义了堆的范围。
 *
 * RAM 布局（直观展示）：
 *   0x28010000  ┌──────────────┐ ← CONFIG_RAM_START
 *               │   .data 段    │   有初值的全局变量（int x = 5;）
 *               ├──────────────┤
 *               │   .bss 段     │   无初值的全局变量（int y; → 默认 0）
 *               ├──────────────┤
 *               │  IDLE 任务栈  │   操作系统空闲任务的栈
 *               ├──────────────┤ ← g_idle_topstack（堆起点）
 *               │              │
 *               │    堆 (Heap)  │   malloc() 从这里分配
 *               │              │
 *   0x28064000  └──────────────┘ ← BK7258_RAM_END = 0x28010000 + 0x54000
 *
 * 堆大小 = RAM 末尾 - 空闲任务栈顶 = 整个 RAM 剩余部分全给 malloc
 *
 * 参考：bk7258_start.c（g_idle_topstack 的定义）
 *       在 Python 中可以看到：fp.read(0x28010000, 0x54000)
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>

#include "arm_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* BK7258_RAM_END：RAM 的结束地址。
 * CONFIG_RAM_START = 0x28010000（ld.script 中 SRAM 的起始地址）
 * CONFIG_RAM_SIZE  = 0x54000（336KB，CPU0 app 的 RAM 窗口大小）
 * 所以 RAM_END = 0x28010000 + 0x54000 = 0x28064000 */

#define BK7258_RAM_END (CONFIG_RAM_START + CONFIG_RAM_SIZE)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* ── up_allocate_heap ──────────────────────────────────────────────────────
 * 告诉 NuttX 内核"堆从哪里开始、有多大"。
 *
 * 堆起点 = g_idle_topstack（空闲任务栈顶，在 bk7258_start.c 中定义）
 *         = _ebss + CONFIG_IDLETHREAD_STACKSIZE
 * 堆大小 = RAM 末尾 - 堆起点 = 整个 RAM 剩余部分。
 *
 * 内核拿到这两个值后，malloc() 就从这里分配内存。 */

void up_allocate_heap(void **heap_start, size_t *heap_size)
{
  *heap_start = (void *)g_idle_topstack;              /* 堆从空闲任务栈顶开始 */
  *heap_size  = BK7258_RAM_END - g_idle_topstack;     /* 剩余 RAM 全给堆 */
}
