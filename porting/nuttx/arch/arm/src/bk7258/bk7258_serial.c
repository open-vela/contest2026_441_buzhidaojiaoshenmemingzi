/****************************************************************************
 * arch/arm/src/bk7258/bk7258_serial.c
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
 * 文件角色：正式串口驱动（中断模式，NuttX 标准 uart_ops 接口）
 *
 * bring-up 流程位置：nx_start() → arm_serialinit() → 本文件
 *
 * 与 bk7258_lowputc.c 的区别：
 *   ┌──────────────────┬─────────────────────┬──────────────────────┐
 *   │                  │ lowputc.c（早期）    │ serial.c（正式）      │
 *   ├──────────────────┼─────────────────────┼──────────────────────┤
 *   │ 调用时机          │ bk7258_cstart() 中  │ nx_start() 之后       │
 *   │ 工作模式          │ 轮询（死等 FIFO）    │ 中断（不占 CPU）       │
 *   │ 能发送            │ ✅                   │ ✅                    │
 *   │ 能接收            │ ❌                   │ ✅                    │
 *   │ 用途              │ 早期调试输出         │ NSH 命令行交互         │
 *   │ 依赖中断系统       │ 不依赖               │ 依赖 up_irqinitialize()│
 *   └──────────────────┴─────────────────────┴──────────────────────┘
 *
 * 关键设计决策：
 *   - RX FIFO 阈值 = 1：收到 1 个字节就触发中断，保证单键输入有响应
 *     否则 bootloader 留下的高阈值（如 60 字节）会让交互输入无响应
 *   - 不做 soft reset：与 lowputc.c 一样，不能破坏 bootloader 的 TX 状态
 *   - 同一个 UART0 注册两个设备名：/dev/console（NSH 用）和 /dev/ttyS0（程序用）
 *
 * 中断处理流程：
 *   你按了一个键 → UART 硬件收到数据 → FIFO 达到阈值 → 触发中断
 *     → bk7258_interrupt() 被调用
 *       → 读中断状态寄存器，看是谁触发的
 *       → 如果是 RX 中断 → 调 uart_recvchars() 把数据读出来
 *       → 如果是 TX 中断 → 调 uart_xmitchars() 把缓冲区的数据发出去
 *       → 写 1 清中断标志（BK7258 UART 是写 1 清除，不是自动清除）
 *
 * 参考：hardware/bk7258_uart.h（UART 寄存器定义）
 *       bk7258_irq.c（中断使能/禁用，两级开关）
 *       bk7258_lowputc.c（早期轮询版本，共用同一套寄存器）
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/serial/serial.h>
#include <arch/board/board.h>

#include "arm_internal.h"
#include "chip.h"
#include "hardware/bk7258_uart.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Console = UART0 (板载 CH340 芯片)。NuttX 作为 CPU0 主核运行，
 * 替换 ARMINO 的 CP app。CPU0 拥有 UART0，bootloader 已经配好了时钟和引脚。
 * 这里只做软配置：115200 8N1，中断号 = EXTINT + 4 = 20。 */

#define CONSOLE_BASE   BK7258_UART0_BASE     /* UART0 基地址 0x44820000 */
#define CONSOLE_IRQ    BK7258_IRQ_UART0      /* UART0 中断号 20 */
#define CONSOLE_BAUD   115200                /* 波特率 115200 */
#define BK7258_UARTCLK 26000000u             /* UART 时钟源 26MHz */

#ifndef CONFIG_UART0_RXBUFSIZE
#  define CONFIG_UART0_RXBUFSIZE 256         /* 接收缓冲区 256 字节 */
#endif
#ifndef CONFIG_UART0_TXBUFSIZE
#  define CONFIG_UART0_TXBUFSIZE 256         /* 发送缓冲区 256 字节 */
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* UART 设备私有数据：每个 UART 端口一个实例。
 *   uartbase: 寄存器基地址
 *   baud:     波特率
 *   irq:      中断号
 *   ie:       中断使能位的影子副本（方便恢复） */

struct bk7258_uart_s
{
  uint32_t uartbase;   /* 寄存器基地址（如 0x44820000） */
  uint32_t baud;       /* 波特率（如 115200） */
  uint8_t  irq;        /* 中断号（如 20） */
  uint8_t  ie;         /* 中断使能影子（记录当前开启了哪些中断） */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  bk7258_setup(struct uart_dev_s *dev);
static void bk7258_shutdown(struct uart_dev_s *dev);
static int  bk7258_attach(struct uart_dev_s *dev);
static void bk7258_detach(struct uart_dev_s *dev);
static int  bk7258_interrupt(int irq, void *context, void *arg);
static int  bk7258_ioctl(struct file *filep, int cmd, unsigned long arg);
static int  bk7258_receive(struct uart_dev_s *dev, unsigned int *status);
static void bk7258_rxint(struct uart_dev_s *dev, bool enable);
static bool bk7258_rxavailable(struct uart_dev_s *dev);
static void bk7258_send(struct uart_dev_s *dev, int ch);
static void bk7258_txint(struct uart_dev_s *dev, bool enable);
static bool bk7258_txready(struct uart_dev_s *dev);
static bool bk7258_txempty(struct uart_dev_s *dev);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* ── 全局数据 ───────────────────────────────────────────────────────────────
 *
 * g_uart_ops: NuttX 串口框架要求的操作函数表，每个函数对应一个串口操作。
 *   框架调用 .setup → 初始化硬件，.send → 发数据，.receive → 收数据 等。
 *
 * g_uart0rxbuffer / g_uart0txbuffer: 收发缓冲区，NuttX 框架管理。
 *   中断模式下数据先写到缓冲区，框架再交给应用程序。
 *
 * g_uart0priv: UART0 的私有数据（基地址、波特率、中断号、中断使能影子）。
 *
 * g_uart0port: UART0 的设备描述符，把缓冲区、操作函数、私有数据绑在一起。
 *    .isconsole = true 表示这也是 NSH 命令行使用的控制台设备。
 * ─────────────────────────────────────────────────────────────────────────── */

static const struct uart_ops_s g_uart_ops =
{
  .setup       = bk7258_setup,        /* 初始化硬件 */
  .shutdown    = bk7258_shutdown,     /* 关闭硬件 */
  .attach      = bk7258_attach,       /* 注册中断处理函数 */
  .detach      = bk7258_detach,       /* 注销中断处理函数 */
  .ioctl       = bk7258_ioctl,        /* 设备控制（如改波特率） */
  .receive     = bk7258_receive,      /* 从 FIFO 读一个字节 */
  .rxint       = bk7258_rxint,        /* 开关接收中断 */
  .rxavailable = bk7258_rxavailable,  /* 检查是否有数据可读 */
  .send        = bk7258_send,         /* 发一个字节到 FIFO */
  .txint       = bk7258_txint,        /* 开关发送中断 */
  .txready     = bk7258_txready,      /* 检查 FIFO 是否可写 */
  .txempty     = bk7258_txempty,      /* 检查 FIFO 是否排空 */
};

static char g_uart0rxbuffer[CONFIG_UART0_RXBUFSIZE];   /* 接收缓冲区 */
static char g_uart0txbuffer[CONFIG_UART0_TXBUFSIZE];   /* 发送缓冲区 */

static struct bk7258_uart_s g_uart0priv =
{
  .uartbase = CONSOLE_BASE,       /* 0x44820000 */
  .baud     = CONSOLE_BAUD,       /* 115200 */
  .irq      = CONSOLE_IRQ,        /* 20 */
  .ie       = 0,                  /* 初始所有中断关闭 */
};

static struct uart_dev_s g_uart0port =
{
  .isconsole = true,              /* 这也是控制台（NSH 用） */
  .recv      =
  {
    .size    = CONFIG_UART0_RXBUFSIZE,     /* 256 */
    .buffer  = g_uart0rxbuffer,
  },
  .xmit      =
  {
    .size    = CONFIG_UART0_TXBUFSIZE,     /* 256 */
    .buffer  = g_uart0txbuffer,
  },
  .ops       = &g_uart_ops,       /* 操作函数表 */
  .priv      = &g_uart0priv,      /* 私有数据 */
};

#define CONSOLE_DEV g_uart0port

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* ── bk7258_restoreint ─────────────────────────────────────────────────────
 * 从影子副本恢复中断使能寄存器。影子（priv->ie）记录了当前应该开启哪些
 * 中断，避免每次开关中断都要读-改-写寄存器。 */

static void bk7258_restoreint(struct bk7258_uart_s *priv)
{
  putreg32(priv->ie, BK7258_UART_INT_ENABLE(priv->uartbase));
}

/* ── bk7258_setup ──────────────────────────────────────────────────────────
 * 初始化 UART 硬件（NuttX 框架在打开串口时调用）。
 *
 * 配置步骤：
 *   1. 配波特率 + 8N1 + TX/RX 使能（不做 soft reset）
 *   2. 配 FIFO：RX 阈值 = 1, RX stop detect = 2
 *   3. 初始化所有中断为关闭状态
 *
 * ⚠️ 不做 soft reset 的原因与 lowputc.c 相同：bootloader 已经配好了 TX 状态。 */

static int bk7258_setup(struct uart_dev_s *dev)
{
  struct bk7258_uart_s *priv = dev->priv;
  uint32_t div = (BK7258_UARTCLK + (priv->baud / 2)) / priv->baud - 1;
  uint32_t cfg;
  uint32_t fc;

  /* 步骤 1：配置波特率、数据位、TX/RX 使能。
   * 只在现有配置上叠加，不动其他位（不做 soft reset）。 */

  cfg  = getreg32(BK7258_UART_CONFIG(priv->uartbase));
  cfg &= ~(UART_CFG_DATA_BITS_MASK | UART_CFG_CLK_DIV_MASK); /* 清旧值 */
  cfg |= UART_CFG_DATA_BITS_8;                                /* 8 数据位 */
  cfg |= (div << UART_CFG_CLK_DIV_SHIFT) & UART_CFG_CLK_DIV_MASK; /* 波特率 */
  cfg |= UART_CFG_TX_ENABLE | UART_CFG_RX_ENABLE;             /* 收发都开 */
  putreg32(cfg, BK7258_UART_CONFIG(priv->uartbase));

  /* 步骤 2：配置 FIFO。
   * fifo_config (0x14): tx_threshold[7:0] | rx_threshold[15:8] | rx_stop_detect[17:16]
   *
   * ⚠️ RX 阈值设为 1（收到 1 个字节就触发中断）是必须的！
   * bootloader 留下的高阈值（如 60 字节）会让交互输入无响应——
   * SWD 实测 rx_fifo_count=60，说明数据堆在 FIFO 里没触发中断。
   *
   * rx_stop_detect=2 表示：RX 空闲 2 个字符时间后触发 RX_FINISH 中断，
   * 用于检测"发完了"（比如一个完整命令行）。 */

  fc  = getreg32(BK7258_UART_FIFO_CFG(priv->uartbase));
  fc &= ~((0xffu << 8) | (0x3u << 16));      /* 清 RX 阈值和 stop detect */
  fc |= (0x01u << 8) | (0x02u << 16);          /* RX 阈值=1, stop=2 */
  putreg32(fc, BK7258_UART_FIFO_CFG(priv->uartbase));

  /* 步骤 3：关闭所有中断，等待框架调用 rxint/txint 来开启 */

  priv->ie = 0;
  bk7258_restoreint(priv);
  return OK;
}

/* ── bk7258_shutdown ───────────────────────────────────────────────────────
 * 关闭串口：关所有中断。 */

static void bk7258_shutdown(struct uart_dev_s *dev)
{
  struct bk7258_uart_s *priv = dev->priv;
  priv->ie = 0;
  bk7258_restoreint(priv);
}

/* ── bk7258_attach ─────────────────────────────────────────────────────────
 * 注册中断处理函数：绑定中断号到 bk7258_interrupt，然后使能中断。
 * 这里调用了 up_enable_irq，它会同时做 NVIC 级和 SYS 级使能。 */

static int bk7258_attach(struct uart_dev_s *dev)
{
  struct bk7258_uart_s *priv = dev->priv;
  int ret = irq_attach(priv->irq, bk7258_interrupt, dev);  /* 绑定 */
  if (ret == OK)
    {
      up_enable_irq(priv->irq);         /* 使能中断（NVIC + SYS 两级） */
    }

  return ret;
}

/* ── bk7258_detach ─────────────────────────────────────────────────────────
 * 注销中断处理函数：禁用中断，然后解绑。 */

static void bk7258_detach(struct uart_dev_s *dev)
{
  struct bk7258_uart_s *priv = dev->priv;
  up_disable_irq(priv->irq);            /* 禁中断（NVIC + SYS 两级） */
  irq_detach(priv->irq);                /* 解绑 */
}

/* ── bk7258_interrupt ──────────────────────────────────────────────────────
 * UART 中断处理函数（被 NVIC 调用）。
 *
 * 处理流程：
 *   1. 读 INT_STATUS 寄存器，看是谁触发了中断
 *   2. 写 1 清除中断标志（⚠️ BK7258 UART 是写 1 清除，不是自动清除）
 *   3. 如果是 RX 中断 → 调 uart_recvchars() 把数据读进缓冲区
 *   4. 如果是 TX 中断 → 调 uart_xmitchars() 把缓冲区的数据发出去
 *
 * 注意：可能同时有 RX 和 TX 中断，所以用 if (status & ...) 而不是 if-else。 */

static int bk7258_interrupt(int irq, void *context, void *arg)
{
  struct uart_dev_s *dev = (struct uart_dev_s *)arg;
  struct bk7258_uart_s *priv = dev->priv;
  uint32_t status;

  status = getreg32(BK7258_UART_INT_STATUS(priv->uartbase));

  /* 写 1 清除：哪个 bit 为 1 就清除哪个中断标志 */
  putreg32(status, BK7258_UART_INT_STATUS(priv->uartbase));

  if (status & (UART_INT_RX_NEED_READ | UART_INT_RX_FINISH))
    {
      uart_recvchars(dev);              /* 收到数据了，读出来 */
    }

  if (status & UART_INT_TX_NEED_WRITE)
    {
      uart_xmitchars(dev);              /* 发送缓冲区空了，填更多数据 */
    }

  return OK;
}

/* ── bk7258_ioctl ──────────────────────────────────────────────────────────
 * 设备控制（如改波特率等）。M1 阶段不支持，返回 -ENOTTY。 */

static int bk7258_ioctl(struct file *filep, int cmd, unsigned long arg)
{
  UNUSED(filep);
  UNUSED(cmd);
  UNUSED(arg);
  return -ENOTTY;
}

/* ── bk7258_receive ────────────────────────────────────────────────────────
 * 从 FIFO 读一个字节。
 * FIFO_PORT 寄存器的低 8 位是发送数据，[15:8] 位是接收数据。 */

static int bk7258_receive(struct uart_dev_s *dev, unsigned int *status)
{
  struct bk7258_uart_s *priv = dev->priv;
  uint32_t rbr = getreg32(BK7258_UART_FIFO_PORT(priv->uartbase));

  *status = 0;                          /* 状态暂不处理 */
  return (int)((rbr >> UART_FIFO_RX_DATA_SHIFT) & 0xff);  /* 取 [15:8] 位 */
}

/* ── bk7258_rxint ──────────────────────────────────────────────────────────
 * 开关接收中断。
 * enable=true  → 开启 RX_NEED_READ + RX_FINISH 中断
 * enable=false → 关闭这两个中断 */

static void bk7258_rxint(struct uart_dev_s *dev, bool enable)
{
  struct bk7258_uart_s *priv = dev->priv;
  irqstate_t flags = enter_critical_section();  /* 关中断，保护临界区 */

  if (enable)
    {
      priv->ie |= UART_INT_RX_NEED_READ | UART_INT_RX_FINISH;
    }
  else
    {
      priv->ie &= ~(UART_INT_RX_NEED_READ | UART_INT_RX_FINISH);
    }

  bk7258_restoreint(priv);              /* 写回硬件寄存器 */
  leave_critical_section(flags);        /* 恢复中断状态 */
}

/* ── bk7258_rxavailable ────────────────────────────────────────────────────
 * 检查接收 FIFO 是否有数据。读 FIFO_STATUS 的 bit21（RD_READY）。 */

static bool bk7258_rxavailable(struct uart_dev_s *dev)
{
  struct bk7258_uart_s *priv = dev->priv;
  return (getreg32(BK7258_UART_FIFO_STATUS(priv->uartbase)) &
          UART_FIFO_RD_READY) != 0;
}

/* ── bk7258_send ───────────────────────────────────────────────────────────
 * 发送一个字节（非阻塞，假设 FIFO 已经可写）。 */

static void bk7258_send(struct uart_dev_s *dev, int ch)
{
  struct bk7258_uart_s *priv = dev->priv;
  putreg32((uint32_t)(uint8_t)ch, BK7258_UART_FIFO_PORT(priv->uartbase));
}

/* ── bk7258_txint ──────────────────────────────────────────────────────────
 * 开关发送中断。
 * enable=true  → 开启 TX_NEED_WRITE 中断，并立即调 uart_xmitchars() 开始发送
 * enable=false → 关闭 TX_NEED_WRITE 中断 */

static void bk7258_txint(struct uart_dev_s *dev, bool enable)
{
  struct bk7258_uart_s *priv = dev->priv;
  irqstate_t flags = enter_critical_section();

  if (enable)
    {
      priv->ie |= UART_INT_TX_NEED_WRITE;
      bk7258_restoreint(priv);

      /* 立即触发一次发送，把缓冲区里的数据发出去。
       * 这叫 "fake a TX interrupt to prime the pump" ——
       * 第一次发送没有硬件中断触发，需要手动推一把。 */
      uart_xmitchars(dev);
    }
  else
    {
      priv->ie &= ~UART_INT_TX_NEED_WRITE;
      bk7258_restoreint(priv);
    }

  leave_critical_section(flags);
}

/* ── bk7258_txready ────────────────────────────────────────────────────────
 * 检查发送 FIFO 是否可写。读 FIFO_STATUS 的 bit20（WR_READY）。 */

static bool bk7258_txready(struct uart_dev_s *dev)
{
  struct bk7258_uart_s *priv = dev->priv;
  return (getreg32(BK7258_UART_FIFO_STATUS(priv->uartbase)) &
          UART_FIFO_WR_READY) != 0;
}

/* ── bk7258_txempty ────────────────────────────────────────────────────────
 * 检查发送 FIFO 是否已排空。读 FIFO_STATUS 的 bit17（TX_EMPTY）。 */

static bool bk7258_txempty(struct uart_dev_s *dev)
{
  struct bk7258_uart_s *priv = dev->priv;
  return (getreg32(BK7258_UART_FIFO_STATUS(priv->uartbase)) &
          UART_FIFO_TX_EMPTY) != 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* ── arm_earlyserialinit ───────────────────────────────────────────────────
 * 早期串口初始化（nx_start 早期调用，在中断系统初始化之前）。
 * 只做硬件配置，不注册中断。此时 lowputc.c 的 bk7258_lowsetup() 已经跑过了，
 * 这里再调一次 setup 确保 RX 也配置好了。 */

void arm_earlyserialinit(void)
{
  CONSOLE_DEV.isconsole = true;         /* 标记为控制台 */
  bk7258_setup(&CONSOLE_DEV);           /* 配置硬件（波特率、数据位、FIFO） */
}

/* ── arm_serialinit ────────────────────────────────────────────────────────
 * 正式串口初始化（nx_start 后期调用，中断系统已就绪）。
 * 注册两个设备名：/dev/console（NSH 用）和 /dev/ttyS0（程序用）。
 * 两个设备名指向同一个物理 UART0，NuttX 框架会处理多路复用。 */

void arm_serialinit(void)
{
  uart_register("/dev/console", &g_uart0port);   /* 控制台设备 */
  uart_register("/dev/ttyS0", &g_uart0port);     /* 通用串口设备 */
}

/* ── up_putc ───────────────────────────────────────────────────────────────
 * NuttX 内核的底层输出函数，直接调 arm_lowputc（轮询模式）。 */

void up_putc(int ch)
{
  arm_lowputc((char)ch);
}
