/****************************************************************************
 * arch/arm/src/bk7258/bk7258_lowputc.c
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
 * 文件角色：早期串口输出（轮询模式，只能发不能收）
 *
 * 这是 bring-up 流程中第二个被调用的模块（第一个是 bk7258_start.c）。
 * 在 bk7258_cstart() 的第 1 步被调用：
 *   bk7258_cstart() → bk7258_lowsetup() → 本文件 → 开 UART0 → showprogress('A')
 *
 * 与 bk7258_serial.c 的关系：
 *   - lowputc.c：启动早期用，轮询模式，简单粗暴，只能发送不能接收
 *   - serial.c：内核启动后用，中断模式，支持收发缓冲，NSH 命令行交互靠它
 *   两者操作同一个物理 UART0，但 lowputc.c 先跑，serial.c 在 nx_start() 后才跑
 *
 * 关键设计决策：
 *   - 不做 soft reset：bootloader 已经把 UART0 配好了，做 reset 会破坏 TX 状态
 *   - 采用轮询发送：简单可靠，不依赖中断系统（此时中断还没初始化）
 *   - UART0 时钟源选 26MHz XTAL：与 bootloader 一致，不会改变波特率
 *   - GPIO10/11 复用为 UART0 RX/TX：bootloader 可能已经拆掉了引脚复用，需重新配置
 *
 * 参考：hardware/bk7258_uart.h（UART 寄存器定义）
 *       hardware/bk7258_memorymap.h（UART0 基地址 = 0x44820000）
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "arm_internal.h"
#include "hardware/bk7258_uart.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 默认使用 UART0 作为调试串口（板载 CH340 芯片连接 UART0）。
 * 下载和日志共用一根 USB 线：bootROM 的 DL_UART 就是 UART0。
 * NuttX 作为 CPU0 主核运行，直接使用这个端口。 */

#ifndef CONFIG_BK7258_CONSOLE_UART_BASE
#  define CONFIG_BK7258_CONSOLE_UART_BASE  BK7258_UART0_BASE
#endif

#ifndef CONFIG_BK7258_CONSOLE_BAUD
#  define CONFIG_BK7258_CONSOLE_BAUD       115200
#endif

/* UART 波特率计算公式（来自 ARMINO SDK 的 uart_ll.c）：
 *    波特率 = 时钟频率 / (分频值 + 1)
 *    分频值 = 时钟频率 / 波特率 - 1
 *
 * 例子：26MHz / 115200 = 225.69 → 取整 225
 *      实际波特率 = 26MHz / 226 = 115044，误差约 0.13%（完全可接受）
 *
 * 加 CONFIG_BK7258_CONSOLE_BAUD/2 是为了四舍五入取整。 */

#define BK7258_UART_CLK   26000000u      /* UART 时钟源：26MHz 晶振 */
#define BK7258_UART_DIV \
  (((BK7258_UART_CLK + (CONFIG_BK7258_CONSOLE_BAUD / 2)) / \
    CONFIG_BK7258_CONSOLE_BAUD) - 1)    /* 波特率分频值，四舍五入 */

#define CONSOLE_BASE      CONFIG_BK7258_CONSOLE_UART_BASE  /* 调试串口基地址 */

/* ── 外设时钟使能 + GPIO 引脚复用 ────────────────────────────────────────
 *
 *   作为 CPU0 主核，NuttX 必须自己开启 UART0 外设时钟并配置 GPIO 引脚为
 *   串口功能。bootloader 在交接时可能已经拆掉了这些配置，不重新配的话写
 *   FIFO 不会有任何输出。
 *
 *   寄存器地址（来自 ARMINO SDK）：
 *     SYS_CLK_EN        (0x44010030): bit2 = UART0 时钟使能
 *     SYS_CLK_DIV_MODE1 (0x44010020): bit10 = UART0 时钟源选择（0=26MHz XTAL）
 *     SYS_GPIO_FUNC8_15 (0x440100C4): GPIO8-15 的功能选择（每 4 位控制一个引脚）
 *     GPIO_CFG(n)       (0x44000400 + n*4): 每个 GPIO 的配置寄存器，bit6=第二功能
 * ─────────────────────────────────────────────────────────────────────────── */

#define BK7258_SYS_CLK_EN        (BK7258_SYS_BASE + 0x30)      /* 0x44010030 */
#  define SYS_CLK_EN_UART0       (1 << 2)  /* bit2: 开 UART0 时钟 */
#define BK7258_SYS_GPIO_FUNC8_15 (BK7258_SYS_BASE + 0xc4)      /* 0x440100C4 */
#define BK7258_SYS_CLK_DIV_MODE1 (BK7258_SYS_BASE + 0x20)      /* 0x44010020 */
#define BK7258_GPIO_CFG(n)       (BK7258_AON_GPIO_BASE + (n) * 4)  /* GPIO 配置寄存器 */
#  define GPIO_CFG_SECOND_FUNC   (1 << 6)  /* bit6: 使能第二功能（外设模式） */

static void bk7258_putc_uart(uintptr_t base, char ch);

/* ── bk7258_uart0_hwsetup ──────────────────────────────────────────────────
 * 开启 UART0 外设时钟、选时钟源为 26MHz XTAL、把 GPIO10/11 复用为 UART0 的
 * RX/TX 引脚。bootloader 在交接时可能拆掉了这些配置，不重新配就没法用串口。
 *
 * 引脚映射：
 *   GPIO11 → UART0_TXD（发送，功能编号 0）
 *   GPIO10 → UART0_RXD（接收，功能编号 0）
 * ─────────────────────────────────────────────────────────────────────────── */

static void bk7258_uart0_hwsetup(void)
{
  /* 步骤 1：使能 UART0 外设时钟。
   * SYS_CLK_EN 寄存器 bit2 置 1 → UART0 的时钟开关打开。 */

  modifyreg32(BK7258_SYS_CLK_EN, 0, SYS_CLK_EN_UART0);

  /* 步骤 2：UART0 时钟源选择 26MHz XTAL。
   * SYS_CLK_DIV_MODE1 寄存器 bit10 清 0 → 选 XTAL（不用 PLL 分频）。
   * 与 bootloader 保持一致，这样波特率不会变。 */

  modifyreg32(BK7258_SYS_CLK_DIV_MODE1, (1u << 10), 0);

  /* 步骤 3：配置 GPIO10 和 GPIO11 的功能编号为 0（UART0）。
   * SYS_GPIO_FUNC8_15 寄存器中：
   *   bits[11:8]   = GPIO10 的功能编号 → 清 0 选 UART0_RXD
   *   bits[15:12]  = GPIO11 的功能编号 → 清 0 选 UART0_TXD */

  modifyreg32(BK7258_SYS_GPIO_FUNC8_15, (0xfu << 8) | (0xfu << 12), 0);

  /* 步骤 4：使能 GPIO10/11 的第二功能（外设模式）。
   * 每个 GPIO 配置寄存器（0x44000400 + n*4）的 bit6 置 1 → 启用外设功能。
   * 如果不置位，GPIO 会保持普通 IO 模式，串口信号出不来。 */

  modifyreg32(BK7258_GPIO_CFG(10), 0, GPIO_CFG_SECOND_FUNC);
  modifyreg32(BK7258_GPIO_CFG(11), 0, GPIO_CFG_SECOND_FUNC);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_lowsetup
 ****************************************************************************/

/* ── bk7258_uart_config ────────────────────────────────────────────────────
 * 配置 UART 的数据位、波特率和 TX 使能。
 *
 * ⚠️ 关键设计：不做 soft reset！
 *   bootloader 已经把 UART0 配好了（分频值=225, TX 已开），正在正常发送数据。
 *   如果做 soft reset 会清除 bootloader 的工作状态，导致 TX 无法排空，
 *   后续的 putc 会超时并丢字符（症状：CH340 只输出一个乱码字节）。
 *   所以只在现有配置基础上叠加 8 数据位 + 波特率 + TX 使能，不动其他位。
 * ─────────────────────────────────────────────────────────────────────────── */

static void bk7258_uart_config(uintptr_t base)
{
  uint32_t cfg;

  /* 读出当前配置，只改数据位、分频值和 TX 使能位，保留其他位不动 */

  cfg  = getreg32(BK7258_UART_CONFIG(base));
  cfg &= ~(UART_CFG_DATA_BITS_MASK | UART_CFG_CLK_DIV_MASK); /* 清掉旧值 */
  cfg |= UART_CFG_DATA_BITS_8;                               /* 8 数据位 */
  cfg |= (BK7258_UART_DIV << UART_CFG_CLK_DIV_SHIFT) & UART_CFG_CLK_DIV_MASK; /* 波特率分频 */
  cfg |= UART_CFG_TX_ENABLE;                                 /* 打开发送 */

  putreg32(cfg, BK7258_UART_CONFIG(base));
}

/* ── bk7258_lowsetup ───────────────────────────────────────────────────────
 * 被 bk7258_cstart() 调用的第一个外设初始化函数。
 * 先做硬件初始化（时钟+引脚），再配 UART 参数（数据位+波特率+TX）。 */

void bk7258_lowsetup(void)
{
  bk7258_uart0_hwsetup();                    /* 开时钟 + 配引脚 */
  bk7258_uart_config(BK7258_UART0_BASE);     /* 配数据位/波特率/TX */
}

/****************************************************************************
 * Name: arm_lowputc
 *
 * Description:
 *   Output one byte on the serial console (polled, blocking).
 *
 ****************************************************************************/

/* ── bk7258_putc_uart ──────────────────────────────────────────────────────
 * 轮询发送一个字符（阻塞，不依赖中断）。
 *
 * 工作流程：
 *   1. 死等 FIFO 可写位（bit20）变为 1（最多等 200000 次循环）
 *   2. 写入 FIFO_PORT 寄存器 → 硬件自动把数据从 TX 引脚发出
 *
 * 超时保护：如果 200000 次循环后 FIFO 还是不可写，就放弃（不写数据）。
 * 这种情况通常意味着 UART 硬件没初始化好。 ────────────────────────────────── */

static void bk7258_putc_uart(uintptr_t base, char ch)
{
  volatile int timeout = 200000;  /* 最大等待次数，防止死循环 */

  /* 轮询等待 FIFO 可写：读 FIFO_STATUS 寄存器，检查 bit20（WR_READY） */
  while (((getreg32(BK7258_UART_FIFO_STATUS(base)) &
           UART_FIFO_WR_READY) == 0) && (--timeout > 0))
    {
    }

  /* 把字符写入 FIFO_PORT → 硬件自动发送 */
  putreg32((uint32_t)(uint8_t)ch, BK7258_UART_FIFO_PORT(base));
}

void arm_lowputc(char ch)
{
  bk7258_putc_uart(CONSOLE_BASE, ch);
}
