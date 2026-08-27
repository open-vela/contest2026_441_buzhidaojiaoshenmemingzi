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

/* Register offsets *********************************************************/

#define BK7258_UART_DEVID_OFFSET       0x0000  /* dev_id */
#define BK7258_UART_VERSION_OFFSET     0x0004  /* dev_version */
#define BK7258_UART_GLOBAL_CTRL_OFFSET 0x0008  /* global_ctrl */
#define BK7258_UART_DEVSTATUS_OFFSET   0x000c  /* dev_status */
#define BK7258_UART_CONFIG_OFFSET      0x0010  /* config */
#define BK7258_UART_FIFO_CFG_OFFSET    0x0014  /* fifo_config */
#define BK7258_UART_FIFO_STATUS_OFFSET 0x0018  /* fifo_status */
#define BK7258_UART_FIFO_PORT_OFFSET   0x001c  /* fifo_port (TX/RX data) */
#define BK7258_UART_INT_ENABLE_OFFSET  0x0020  /* int_enable */
#define BK7258_UART_INT_STATUS_OFFSET  0x0024  /* int_status */
#define BK7258_UART_FLOW_CFG_OFFSET    0x0028  /* flow_ctrl_config */
#define BK7258_UART_WAKE_CFG_OFFSET    0x002c  /* wake_config */

/* Register addresses (per UART instance) ***********************************/

#define BK7258_UART_GLOBAL_CTRL(b)     ((b) + BK7258_UART_GLOBAL_CTRL_OFFSET)
#define BK7258_UART_CONFIG(b)          ((b) + BK7258_UART_CONFIG_OFFSET)
#define BK7258_UART_FIFO_CFG(b)        ((b) + BK7258_UART_FIFO_CFG_OFFSET)
#define BK7258_UART_FIFO_STATUS(b)     ((b) + BK7258_UART_FIFO_STATUS_OFFSET)
#define BK7258_UART_FIFO_PORT(b)       ((b) + BK7258_UART_FIFO_PORT_OFFSET)
#define BK7258_UART_INT_ENABLE(b)      ((b) + BK7258_UART_INT_ENABLE_OFFSET)
#define BK7258_UART_INT_STATUS(b)      ((b) + BK7258_UART_INT_STATUS_OFFSET)

/* GLOBAL_CTRL (0x08) bit definitions ***************************************/

#define UART_GLOBAL_SOFT_RESET         (1 << 0)  /* bit0: uart soft reset */
#define UART_GLOBAL_CLK_GATE_BYPASS    (1 << 1)  /* bit1: bypass clock gate */

/* CONFIG (0x10) bit definitions ********************************************/

#define UART_CFG_TX_ENABLE             (1 << 0)  /* bit0: tx enable */
#define UART_CFG_RX_ENABLE             (1 << 1)  /* bit1: rx enable */
#define UART_CFG_DATA_BITS_SHIFT       3         /* bits[3:4]: data bits */
#define UART_CFG_DATA_BITS_MASK        (0x3 << UART_CFG_DATA_BITS_SHIFT)
#  define UART_CFG_DATA_BITS_5         (0x0 << UART_CFG_DATA_BITS_SHIFT)
#  define UART_CFG_DATA_BITS_6         (0x1 << UART_CFG_DATA_BITS_SHIFT)
#  define UART_CFG_DATA_BITS_7         (0x2 << UART_CFG_DATA_BITS_SHIFT)
#  define UART_CFG_DATA_BITS_8         (0x3 << UART_CFG_DATA_BITS_SHIFT)
#define UART_CFG_PARITY_EN             (1 << 5)  /* bit5: parity enable */
#define UART_CFG_PARITY_ODD            (1 << 6)  /* bit6: 0=even,1=odd */
#define UART_CFG_STOP_BITS_2           (1 << 7)  /* bit7: 0=1bit,1=2bit */
#define UART_CFG_CLK_DIV_SHIFT         8         /* bits[8:23]: clk_div */
#define UART_CFG_CLK_DIV_MASK          (0xffff << UART_CFG_CLK_DIV_SHIFT)

/* FIFO_STATUS (0x18) bit definitions ***************************************/

#define UART_FIFO_TX_COUNT_SHIFT       0         /* bits[0:7] */
#define UART_FIFO_TX_COUNT_MASK        (0xff << 0)
#define UART_FIFO_RX_COUNT_SHIFT       8         /* bits[8:15] */
#define UART_FIFO_RX_COUNT_MASK        (0xff << 8)
#define UART_FIFO_TX_FULL              (1 << 16) /* bit16 */
#define UART_FIFO_TX_EMPTY             (1 << 17) /* bit17 */
#define UART_FIFO_RX_FULL              (1 << 18) /* bit18 */
#define UART_FIFO_RX_EMPTY             (1 << 19) /* bit19 */
#define UART_FIFO_WR_READY             (1 << 20) /* bit20: can write TX FIFO */
#define UART_FIFO_RD_READY             (1 << 21) /* bit21: RX FIFO has data */

/* FIFO_PORT (0x1c): writing (v & 0xff) sends one byte; the low 8 bits of a
 * read hold the received byte.  Ref uart_ll_write_byte():
 * hw->fifo_port.v = data & 0xff;
 */

#define UART_FIFO_RX_DATA_SHIFT        8         /* rx_fifo_data_out [8:15] */
#define UART_FIFO_RX_DATA_MASK         (0xff << 8)

/* INT_ENABLE (0x20) / INT_STATUS (0x24) bit definitions ********************/

#define UART_INT_TX_NEED_WRITE         (1 << 0)  /* bit0: TX FIFO writable */
#define UART_INT_RX_NEED_READ          (1 << 1)  /* bit1: RX FIFO has data */
#define UART_INT_RX_OVERFLOW           (1 << 2)  /* bit2: RX FIFO overflow */
#define UART_INT_RX_PARITY_ERR         (1 << 3)  /* bit3 */
#define UART_INT_RX_STOP_ERR           (1 << 4)  /* bit4 */
#define UART_INT_TX_FINISH             (1 << 5)  /* bit5 */
#define UART_INT_RX_FINISH             (1 << 6)  /* bit6: RX idle (stop) */
#define UART_INT_RXD_WAKEUP            (1 << 7)  /* bit7 */

#endif /* __ARCH_ARM_SRC_BK7258_HARDWARE_BK7258_UART_H */
