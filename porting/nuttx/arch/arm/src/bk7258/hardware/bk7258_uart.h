/****************************************************************************
 * arch/arm/src/bk7258/hardware/bk7258_uart.h
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
 * 文件角色：UART 寄存器的"户型图"（偏移量 + 每个 bit 的含义）
 *
 * 通俗理解：memorymap.h 告诉了你楼的地址（比如 UART0 在 0x44820000），
 * 这个文件告诉你楼里每个房间的编号（偏移量）和房间里的开关怎么用（bit 含义）。
 *
 * 寄存器布局参考 ARMINO SDK 的 uart_hw_t 结构体，每个寄存器 32 位，连续排列：
 *
 *   偏移量     寄存器名         通俗理解
 *   ──────────────────────────────────────────────────
 *   0x00       DEVID            芯片 ID（只读）
 *   0x04       VERSION          版本号（只读）
 *   0x08       GLOBAL_CTRL      全局控制（bit0=软件复位, bit1=时钟旁路）
 *   0x0C       DEVSTATUS        设备状态（只读）
 *   0x10       CONFIG           配置寄存器（波特率、数据位、TX/RX 使能）
 *   0x14       FIFO_CFG         FIFO 配置（RX 阈值、TX 阈值）
 *   0x18       FIFO_STATUS      FIFO 状态（是否可读、是否可写、是否满）
 *   0x1C       FIFO_PORT        FIFO 数据端口（写 = 发送, 读 = 接收）
 *   0x20       INT_ENABLE       中断使能（哪些事件触发中断）
 *   0x24       INT_STATUS       中断状态（当前哪些中断在等待处理）
 *   0x28       FLOW_CFG         流控配置
 *   0x2C       WAKE_CFG         唤醒配置
 *
 * 最常用的寄存器：
 *   - CONFIG (0x10)：设置波特率、数据位、TX/RX 开关
 *   - FIFO_STATUS (0x18)：检查发送 FIFO 是否可写、接收 FIFO 是否有数据
 *   - FIFO_PORT (0x1C)：写入一个字节 = 发送，读出一个字节 = 接收
 *   - INT_ENABLE (0x20)：使能 RX/TX 中断
 *   - INT_STATUS (0x24)：读中断状态，写 1 清除（⚠️ BK7258 的 UART 是写 1 清中断）
 *
 * 参考：ARMINO SDK: ap/middleware/soc/bk7258_ap/soc/uart_struct.h (uart_hw_t)
 *       bk7258_memorymap.h（UART 基地址定义）
 *       bk7258_lowputc.c / bk7258_serial.c（实际使用这些寄存器的代码）
 ****************************************************************************/

/* Register layout from ARMINO SDK
 * ap/middleware/soc/bk7258_ap/soc/uart_struct.h (uart_hw_t): each register
 * is 32-bit and laid out contiguously.
 */

#ifndef __ARCH_ARM_SRC_BK7258_HARDWARE_BK7258_UART_H
#define __ARCH_ARM_SRC_BK7258_HARDWARE_BK7258_UART_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "bk7258_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ── 寄存器偏移量 ───────────────────────────────────────────────────────────
 * 每个寄存器 32 位，连续排列。偏移量 = 寄存器在 uart_hw_t 结构体中的位置。
 * 完整地址 = 基地址 + 偏移量，例如 UART0 的 CONFIG 寄存器 = 0x44820000 + 0x10 */

#define BK7258_UART_DEVID_OFFSET       0x0000  /* 设备 ID（只读） */
#define BK7258_UART_VERSION_OFFSET     0x0004  /* 版本号（只读） */
#define BK7258_UART_GLOBAL_CTRL_OFFSET 0x0008  /* 全局控制：bit0=软件复位, bit1=时钟旁路 */
#define BK7258_UART_DEVSTATUS_OFFSET   0x000c  /* 设备状态（只读） */
#define BK7258_UART_CONFIG_OFFSET      0x0010  /* 配置：波特率、数据位、TX/RX使能 */
#define BK7258_UART_FIFO_CFG_OFFSET    0x0014  /* FIFO配置：RX/TX阈值 */
#define BK7258_UART_FIFO_STATUS_OFFSET 0x0018  /* FIFO状态：可读、可写、满、空 */
#define BK7258_UART_FIFO_PORT_OFFSET   0x001c  /* FIFO数据端口：写=发送, 读=接收 */
#define BK7258_UART_INT_ENABLE_OFFSET  0x0020  /* 中断使能：哪些事件触发中断 */
#define BK7258_UART_INT_STATUS_OFFSET  0x0024  /* 中断状态：写1清除 */
#define BK7258_UART_FLOW_CFG_OFFSET    0x0028  /* 流控配置 */
#define BK7258_UART_WAKE_CFG_OFFSET    0x002c  /* 唤醒配置 */

/* ── 寄存器地址宏（基地址 + 偏移量）─────────────────────────────────────── */

#define BK7258_UART_GLOBAL_CTRL(b)     ((b) + BK7258_UART_GLOBAL_CTRL_OFFSET)
#define BK7258_UART_CONFIG(b)          ((b) + BK7258_UART_CONFIG_OFFSET)
#define BK7258_UART_FIFO_CFG(b)        ((b) + BK7258_UART_FIFO_CFG_OFFSET)
#define BK7258_UART_FIFO_STATUS(b)     ((b) + BK7258_UART_FIFO_STATUS_OFFSET)
#define BK7258_UART_FIFO_PORT(b)       ((b) + BK7258_UART_FIFO_PORT_OFFSET)
#define BK7258_UART_INT_ENABLE(b)      ((b) + BK7258_UART_INT_ENABLE_OFFSET)
#define BK7258_UART_INT_STATUS(b)      ((b) + BK7258_UART_INT_STATUS_OFFSET)

/* ── GLOBAL_CTRL (0x08) 位定义 ───────────────────────────────────────────── */

#define UART_GLOBAL_SOFT_RESET         (1 << 0)  /* bit0: 软件复位（⚠️ 不能用，会破坏bootloader状态） */
#define UART_GLOBAL_CLK_GATE_BYPASS    (1 << 1)  /* bit1: 时钟门控旁路 */

/* ── CONFIG (0x10) 位定义 ──────────────────────────────────────────────────
 * 这是最常用的配置寄存器，控制波特率、数据位、TX/RX开关。
 * 波特率 = 时钟频率 / (clk_div + 1)，clk_div 在 bits[23:8]。 */

#define UART_CFG_TX_ENABLE             (1 << 0)  /* bit0: 打开发送 */
#define UART_CFG_RX_ENABLE             (1 << 1)  /* bit1: 打开接收 */
#define UART_CFG_DATA_BITS_SHIFT       3         /* bits[3:4]: 数据位数 */
#define UART_CFG_DATA_BITS_MASK        (0x3 << UART_CFG_DATA_BITS_SHIFT)
#  define UART_CFG_DATA_BITS_5         (0x0 << UART_CFG_DATA_BITS_SHIFT)  /* 5位 */
#  define UART_CFG_DATA_BITS_6         (0x1 << UART_CFG_DATA_BITS_SHIFT)  /* 6位 */
#  define UART_CFG_DATA_BITS_7         (0x2 << UART_CFG_DATA_BITS_SHIFT)  /* 7位 */
#  define UART_CFG_DATA_BITS_8         (0x3 << UART_CFG_DATA_BITS_SHIFT)  /* 8位 */
#define UART_CFG_PARITY_EN             (1 << 5)  /* bit5: 校验使能 */
#define UART_CFG_PARITY_ODD            (1 << 6)  /* bit6: 0=偶校验, 1=奇校验 */
#define UART_CFG_STOP_BITS_2           (1 << 7)  /* bit7: 0=1停止位, 1=2停止位 */
#define UART_CFG_CLK_DIV_SHIFT         8         /* bits[8:23]: 波特率分频值 */
#define UART_CFG_CLK_DIV_MASK          (0xffff << UART_CFG_CLK_DIV_SHIFT)

/* ── FIFO_STATUS (0x18) 位定义 ───────────────────────────────────────────── */
/* 最常用的三个位：WR_READY（可写）、RD_READY（可读）、TX_EMPTY（已排空） */

#define UART_FIFO_TX_COUNT_SHIFT       0         /* bits[0:7]: TX FIFO 中待发送的字节数 */
#define UART_FIFO_TX_COUNT_MASK        (0xff << 0)
#define UART_FIFO_RX_COUNT_SHIFT       8         /* bits[8:15]: RX FIFO 中已接收的字节数 */
#define UART_FIFO_RX_COUNT_MASK        (0xff << 8)
#define UART_FIFO_TX_FULL              (1 << 16) /* bit16: TX FIFO 满了 */
#define UART_FIFO_TX_EMPTY             (1 << 17) /* bit17: TX FIFO 空了（数据全部发送完毕） */
#define UART_FIFO_RX_FULL              (1 << 18) /* bit18: RX FIFO 满了 */
#define UART_FIFO_RX_EMPTY             (1 << 19) /* bit19: RX FIFO 空了 */
#define UART_FIFO_WR_READY             (1 << 20) /* bit20: TX FIFO 可写（最常用） */
#define UART_FIFO_RD_READY             (1 << 21) /* bit21: RX FIFO 有数据可读（最常用） */

/* ── FIFO_PORT (0x1c) 位定义 ───────────────────────────────────────────────
 * 写操作：低 8 位 (v & 0xff) 发送一个字节
 * 读操作：bits[15:8] 是接收到的数据 */

#define UART_FIFO_RX_DATA_SHIFT        8         /* 接收数据在 bits[15:8] */
#define UART_FIFO_RX_DATA_MASK         (0xff << 8)

/* ── INT_ENABLE (0x20) / INT_STATUS (0x24) 位定义 ──────────────────────────
 * 这两个寄存器使用相同的位定义。
 * INT_ENABLE：写 1 使能中断，写 0 禁用
 * INT_STATUS：读 1 表示该中断已触发，写 1 清除中断标志 */

#define UART_INT_TX_NEED_WRITE         (1 << 0)  /* bit0: TX FIFO 可写，需要填数据 */
#define UART_INT_RX_NEED_READ          (1 << 1)  /* bit1: RX FIFO 有数据，需要读走 */
#define UART_INT_RX_OVERFLOW           (1 << 2)  /* bit2: RX FIFO 溢出（数据丢了） */
#define UART_INT_RX_PARITY_ERR         (1 << 3)  /* bit3: 校验错误 */
#define UART_INT_RX_STOP_ERR           (1 << 4)  /* bit4: 停止位错误 */
#define UART_INT_TX_FINISH             (1 << 5)  /* bit5: TX 发送完毕 */
#define UART_INT_RX_FINISH             (1 << 6)  /* bit6: RX 空闲（接收暂停） */
#define UART_INT_RXD_WAKEUP            (1 << 7)  /* bit7: RX 数据唤醒 */

#endif /* __ARCH_ARM_SRC_BK7258_HARDWARE_BK7258_UART_H */
