/****************************************************************************
 * arch/arm/src/bk7258/bk7258_start.c
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
 * 文件角色：bootloader 交接后，BK7258 上执行的 NuttX 第一段代码
 *
 * 整个 bring-up 流程的起点：bootloader 读完分区表 → 加载 nuttx 镜像 → 跳转到
 * 这里的 __start() → C 启动代码 → 最终进入 NuttX 内核 nx_start()。
 *
 * 调用链（按执行顺序）：
 *   __start()                    ← 汇编裸函数，清理硬件状态
 *     └─ bk7258_cstart()         ← C 语言启动主体
 *          ├─ bk7258_lowsetup()       → bk7258_lowputc.c（打开串口）
 *          ├─ bk7258_wdt_disable()    → 本文件（关看门狗，防止 boot loop）
 *          ├─ 清零 .bss               → 全局变量归零
 *          ├─ 搬运 .data              → 初值从 Flash 拷到 RAM
 *          ├─ arm_fpuconfig()         → arm 通用代码（开浮点运算）
 *          ├─ arm_boardinitialize()   → 板级初始化（目前空函数）
 *          └─ nx_start()              → 进入 NuttX 内核
 *               ├─ up_timer_initialize()  → bk7258_timerisr.c（系统心跳）
 *               ├─ up_irqinitialize()     → bk7258_irq.c（中断系统）
 *               ├─ up_allocate_heap()     → bk7258_allocateheap.c（malloc 内存）
 *               └─ arm_serialinit()       → bk7258_serial.c（正式串口驱动）
 *
 * 启动阶段里程碑（通过 showprogress 输出字母，调试时看最后一个字母定位卡死点）：
 *   A: 串口初始化完成，能说话了
 *   B: 看门狗已关，不会重启了
 *   C: .bss 清零完成，全局变量已归零
 *   D: .data 搬运完成，有初值的全局变量就绪
 *   E: FPU 配置完成，浮点运算可用
 *   F: 板级初始化完成，即将进入 nx_start()
 *   \r\n + 进入 NuttX 内核调度
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/init.h>
#include <arch/board/board.h>

#include "arm_internal.h"
#include "nvic.h"

#include "chip.h"
#include "hardware/bk7258_memorymap.h"
#include "bk7258_lowputc.h"
#include "bk7258_start.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Emit single-character start-up milestones to the console only when
 * CONFIG_DEBUG_FEATURES is set; a no-op otherwise.  Useful for early
 * bring-up to locate where start-up stalls.
 */

#ifdef CONFIG_DEBUG_FEATURES
#  define showprogress(c) arm_lowputc(c)
#else
#  define showprogress(c)
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* g_idle_topstack: top of the idle thread stack = end of .bss (_ebss) plus
 * the idle stack size.  The heap starts right after it (see
 * bk7258_allocateheap.c).
 */

const uintptr_t g_idle_topstack =
  (uintptr_t)_ebss + CONFIG_IDLETHREAD_STACKSIZE;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: __start
 *
 * Description:
 *   NuttX 镜像接管后的第一条指令（汇编裸函数，naked 意味着编译器不生成函数序言，
 *   所有寄存器操作由我们手动完成）。
 *
 *   ⚠️ 关键背景：这不是"真复位"，而是 bootloader 的"跳转"
 *   ─────────────────────────────────────────────────────────
 *   芯片上电 → bootloader 先跑 → 初始化 Flash/时钟 → 跳转到我们的程序。
 *   "跳转"不等于"复位"——bootloader 用过的脏数据（寄存器、栈上限、中断表）
 *   还留在芯片里，必须手动清理，否则翻车。
 *
 *   操作顺序（沿用公开候选实现的 SWD 单步结论，顺序不能乱）：
 *   ┌─────────────────────────────────────────────────────┐
 *   │  (1) 关中断         — 我没准备好，别来打扰我        │
 *   │  (2) 清 MSPLIM       — 拆掉 bootloader 的限高杆      │
 *   │  (3) 开 FPU          — 打开浮点运算器                 │
 *   │  (4) 设栈指针 SP     — 从向量表[0] 取栈地址           │
 *   │  (5) 设向量表 VTOR   — 指向我们的中断向量表            │
 *   │  (6) 跳入 C 代码     — b bk7258_cstart               │
 *   └─────────────────────────────────────────────────────┘
 *
 *   为什么顺序不能乱？
 *   - 如果先设 SP 再清 MSPLIM：SP 低于旧 MSPLIM 值 → 芯片误判"栈溢出"
 *     → UsageFault(STKOF) → 死机没输出
 *   - 如果先设 SP 再开 FPU：异常发生时 FPU 未就绪 → NOCP 故障
 *     → 双重故障(NOCP+STKOF) → LOCKUP → 芯片彻底锁死
 *
 *   参考：arch/arm/src/arm_m/arm_vectors.c（向量表定义）
 *         vendor/beken/boards/bk7258/bk7258-r1/scripts/ld.script（链接脚本）
 ****************************************************************************/

void __start(void) naked_function;
void __start(void)
{
  __asm__ __volatile__
  (
    /* ── 步骤1：关中断 ───────────────────────────────── */
    "  cpsid i\n"                 /* 全局关中断（CPU 不再响应任何中断） */

    /* ── 步骤2：清 MSPLIM（栈上限寄存器）─────────────── */
    "  movs  r0, #0\n"
    "  msr   MSPLIM, r0\n"        /* MSPLIM=0，相当于拆掉限高杆 */
    /* 通俗理解：MSPLIM 是 Armv8-M 的"栈天花板"功能。
     *          bootloader 设了一个很高的值（比如 0x28020000），
     *          而我们程序的栈在 0x28011c68 附近。如果不先清零，
     *          后续任何栈操作（push/pop）都可能触发 STKOF 故障。 */

    /* ── 步骤3：开 FPU（浮点运算器）───────────────────── */
    "  ldr   r0, =0xE000ED88\n"   /* SCB->CPACR 寄存器地址 */
    "  ldr   r1, [r0]\n"
    "  ldr   r2, =0x00f00000\n"   /* CP10=3, CP11=3 → 浮点寄存器全权限 */
    "  orr   r1, r1, r2\n"
    "  str   r1, [r0]\n"
    "  dsb\n"                     /* 数据同步屏障，确保写操作完成 */
    "  isb\n"                     /* 指令同步屏障，确保后续指令看到新值 */
    /* 通俗理解：FPU 是芯片的"数学加速器"，专门处理小数运算。
     *          默认是关的。如果不提前打开，等发生中断时，
     *          硬件要保存浮点寄存器 → 发现 FPU 没开 → NOCP 故障 → 死机。 */

    /* ── 步骤4：设置栈指针 SP ────────────────────────── */
    "  ldr   r0, =_vectors\n"     /* 取向量表首地址 */
    "  ldr   r1, [r0]\n"          /* 读向量表[0] = 初始栈指针值 */
    "  mov   sp, r1\n"            /* 设置 SP（主栈指针 MSP） */
    /* 通俗理解：栈是程序运行的工作台，SP 指向工作台顶部。
     *          _vectors[0] 的值在链接脚本中定义，等于 _ebss + 空闲任务栈大小。 */

    /* ── 步骤5：设置中断向量表 VTOR ──────────────────── */
    "  ldr   r0, =_vectors\n"     /* 向量表地址 */
    "  ldr   r1, =0xE000ED08\n"   /* SCB->VTOR 寄存器地址 */
    "  str   r0, [r1]\n"          /* 把向量表地址写入 VTOR */
    /* 通俗理解：向量表是一本"电话簿"，记录了每个中断的处理函数地址。
     *          VTOR 寄存器告诉 CPU "电话簿放在哪里"。
     *          如果不设置，CPU 会继续用 bootloader 的旧电话簿，
     *          中断来了会跳到 bootloader 的处理函数（可能已经不存在了）。 */

    /* ── 步骤6：跳入 C 语言启动代码 ───────────────────── */
    "  b     bk7258_cstart\n"     /* 汇编阶段结束，进入 C 语言的世界 */
  );
}

/****************************************************************************
 * Name: bk7258_wdt_disable
 *
 * Description:
 *   关闭两个看门狗（AON WDT 和 Normal WDT）。
 *
 *   通俗理解：看门狗是一个"定时炸弹"倒计时器。bootloader 设了一个约 100ms
 *   的短倒计时，如果到时间还没有程序去"喂狗"（重置倒计时），芯片就自动重启。
 *   如果不赶紧关掉，系统就会陷入"启动→超时→重启→启动→超时..."的死循环。
 *
 *   关闭方式参考 ARMINO SDK 的 wdt_hal_close()：
 *     SOC_AON_WDT_REG_BASE = 0x44000600
 *     SOC_WDT_REG_BASE     = 0x44800000
 ****************************************************************************/

static void bk7258_wdt_disable(void)
{
  uint32_t v;

  /* AON WDT: write key (0x5A/0xA5) << 16, period = 0 */

  putreg32(0x5a0000, BK7258_AON_WDT_BASE);
  putreg32(0xa50000, BK7258_AON_WDT_BASE);

  /* Normal WDT: global_ctrl (off 0x08) bit1 = 1, then ctrl (off 0x10)
   * write key, period = 0.
   */

  v = getreg32(BK7258_WDT_BASE + 0x08);
  putreg32(v | (1u << 1), BK7258_WDT_BASE + 0x08);
  putreg32(0x5a0000, BK7258_WDT_BASE + 0x10);
  putreg32(0xa50000, BK7258_WDT_BASE + 0x10);
}

/****************************************************************************
 * Name: bk7258_cstart
 *
 * Description:
 *   C 语言启动主体，从 __start() 的汇编代码跳转过来。
 *
 *   执行顺序（每一步都输出一个字母作为调试标记）：
 *     A: 初始化串口（调用 bk7258_lowsetup → bk7258_lowputc.c）
 *     B: 关看门狗（防止 boot loop）
 *     C: 清零 .bss 段（全局变量归零）
 *     D: 搬运 .data 段（初值从 Flash 拷到 RAM）
 *     E: 配置 FPU（浮点运算器）
 *     F: 板级初始化（arm_boardinitialize，目前空函数）
 *     \r\n: 进入 nx_start() → NuttX 内核
 *
 *   .bss 和 .data 的区别（通俗理解）：
 *     - .bss：没初始值的全局变量（比如 int x;），C 标准规定默认是 0，
 *             但 RAM 刚上电是随机值，所以必须手动清零。
 *     - .data：有初始值的全局变量（比如 int x = 5;），
 *             初始值 5 存在 Flash 里（省电不丢），但程序运行时变量必须
 *             在 RAM 里（读写快），所以要从 Flash 拷到 RAM。
 *
 *   参考：vendor/beken/boards/bk7258/bk7258-r1/scripts/ld.script
 *         定义了 _sbss, _ebss, _sdata, _edata, _eronly 等符号
 ****************************************************************************/

void bk7258_cstart(void) noreturn_function;
void bk7258_cstart(void)
{
  const uint32_t *src;
  uint32_t *dest;

  /* ── 步骤 A：打开串口，让芯片能说话 ─────────────────── */
  /* 调用 bk7258_lowsetup() → bk7258_lowputc.c
   * 使能 UART0 时钟 + 配置 GPIO10/11 为串口引脚 + 配 115200 8N1 */

  bk7258_lowsetup();
  showprogress('A');

  /* ── 步骤 B：关看门狗，拆掉定时炸弹 ─────────────────── */
  /* 必须在 ~100ms 内完成，否则芯片自动重启 */

  bk7258_wdt_disable();
  showprogress('B');

  /* ── 步骤 C：清零 .bss 段 ──────────────────────────── */
  /* 遍历 _sbss 到 _ebss 的每个字节，写 0。
   * 通俗理解：把所有"没初始值的全局变量"归零。 */

  for (dest = (uint32_t *)_sbss; dest < (uint32_t *)_ebss; dest++)
    {
      *dest = 0;
    }

  showprogress('C');

  /* ── 步骤 D：搬运 .data 段初值 ─────────────────────── */
  /* 从 Flash 读区（_eronly 即 lma）把初始值逐个拷贝到 RAM（_sdata 即 vma）。
   * 通俗理解：Flash 是仓库，RAM 是工位。有初值的全局变量（比如 int x=5），
   *          初始值 5 存在仓库（Flash），但运行时要在工位（RAM）操作，
   *          所以要把值从仓库搬到工位。 */

  for (src = (const uint32_t *)_eronly, dest = (uint32_t *)_sdata;
       dest < (uint32_t *)_edata;
       dest++, src++)
    {
      *dest = *src;
    }

  showprogress('D');

  /* ── 步骤 E：配置 FPU（浮点运算器）─────────────────── */

#ifdef CONFIG_ARCH_FPU
  arm_fpuconfig();
#endif

  showprogress('E');

  /* ── 步骤 F：板级初始化 ────────────────────────────── */
  /* 目前是空函数，在 vendor/beken/boards/bk7258/bk7258-r1/src/
   * bk7258_boardinitialize.c 中实现。预留用于 GPIO、LCD 背光等板级初始化。 */

  arm_boardinitialize();
  showprogress('F');

  showprogress('\r');
  showprogress('\n');

  /* ── 最后一步：进入 NuttX 操作系统内核 ──────────────── */
  /* nx_start() 是 NuttX 的内核入口，它会：
   *   1. 初始化数据结构
   *   2. 创建 IDLE 线程 + INIT 线程
   *   3. 调用 up_timer_initialize() → bk7258_timerisr.c（启动系统心跳）
   *   4. 调用 up_irqinitialize() → bk7258_irq.c（初始化中断系统）
   *   5. 调用 up_allocate_heap() → bk7258_allocateheap.c（设置 malloc 内存）
   *   6. 调用 arm_serialinit() → bk7258_serial.c（注册正式串口驱动）
   *   7. 启动调度器 → NSH 命令行就绪
   * nx_start() 理论上永远不会返回，如果返回了说明发生了严重错误。 */

  nx_start();

  /* 兜底：万一 nx_start() 返回了，原地死循环，别让 CPU 跑到未知的地方去 */

  for (; ; )
    {
    }
}
