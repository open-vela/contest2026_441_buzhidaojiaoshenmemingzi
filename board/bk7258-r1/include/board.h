/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-r1/include/board.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/
/****************************************************************************
 * 文件角色：R1 板子的"身份证"（板级配置头文件）
 *
 * 这个文件定义了 R1 板子的所有硬件参数，被芯片层（nuttx/arch/arm/src/bk7258/）
 * 通过 #include <arch/board/board.h> 引用。
 *
 * 为什么需要这个文件？
 *   - 同一颗 BK7258 芯片可以焊在不同的板子上，每块板子的晶振频率、
 *     内存大小、引脚分配可能不同
 *   - 芯片层代码只管"这颗芯片怎么用"，具体参数从板级头文件读取
 *   - 换一块板子只需要换这个头文件，芯片层代码完全不用改
 *
 * 与 bk7258-devkit 的关系：
 *   - bk7258-devkit 是公开候选实现的开发板（作者报告已完成 bring-up）
 *   - bk7258-r1 是定制板（基于 devkit 修改，时钟/内存窗口更保守）
 *   - R1 继承了 devkit 的芯片层代码，只修改了板级配置
 *
 * 参考：nuttx/arch/arm/src/bk7258/bk7258_start.c（引用 board.h 的 arm_boardinitialize）
 *       本目录下的 scripts/ld.script（链接脚本，内存布局与这里呼应）
 ****************************************************************************/

#ifndef __BOARDS_BK7258_R1_INCLUDE_BOARD_H
#define __BOARDS_BK7258_R1_INCLUDE_BOARD_H

#include <nuttx/config.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * 时钟配置
 *
 * 通俗理解：芯片运行时需要一个"节拍器"来同步所有操作。这个节拍器就是晶振。
 *   - XTAL（晶振）= 26MHz：板子上焊的物理晶振，所有时钟的源头
 *   - CPU 频率 = 240MHz：芯片内部通过 PLL（锁相环）把 26MHz 倍频到 240MHz
 *   - SysTick 时钟 = CPU 频率：系统心跳用 CPU 时钟做时钟源
 *
 *   ⚠️ 当前 240MHz 来自 R1/AIDK CPU0 配置，不是本队实测值。
 *   拿到 R1 后必须核对 bootloader 实际留下的 CPU 时钟；若不一致，SysTick
 *   节拍也会随之错误。
 * ═══════════════════════════════════════════════════════════════════════════ */

#define BOARD_XTAL_FREQUENCY   26000000      /* 板载晶振：26MHz */
#define BOARD_CPU_FREQUENCY    240000000     /* CPU 运行频率：240MHz */
#define BOARD_SYSTICK_CLOCK    BOARD_CPU_FREQUENCY  /* SysTick 时钟源 = CPU 频率 */

/* ═══════════════════════════════════════════════════════════════════════════
 * 内存窗口定义
 *
 * 通俗理解：芯片上有好几种"仓库"（内存），每种仓库有自己的门牌号范围。
 *
 *   内存类型     基地址         大小          通俗理解
 *   ─────────────────────────────────────────────────────
 *   ITCM        0x00000000     -             指令紧耦合内存（CPU0 私有，极快）
 *   DTCM        0x20000000     -             数据紧耦合内存（CPU0 私有，极快）
 *   Flash       0x02000000     8MB           程序存储器（XIP，可直接执行代码）
 *   SRAM        0x28000000     640KB         共享内存（CPU0/CPU1 都可以访问）
 *   PSRAM       0x60000000     16MB          外挂伪静态内存（大容量，较慢）
 *
 *   ⚠️ M1 阶段只用了 SRAM 的一个子窗口（0x28010000, 336KB），
 *   不是全部 640KB。因为 Secure 固件、boot 握手状态、后续 AP 核可能占用一部分。
 *
 *   ld.script 中定义的 RAM 窗口（0x28010000, 0x54000）与这里呼应。
 * ═══════════════════════════════════════════════════════════════════════════ */

#define BOARD_SRAM_BASE        0x28000000    /* 共享 SRAM 起始地址 */
#define BOARD_SRAM_SIZE        (640 * 1024)  /* 共享 SRAM 总大小：640KB */
#define BOARD_ITCM_BASE        0x00000000    /* 指令 TCM 起始地址 */
#define BOARD_DTCM_BASE        0x20000000    /* 数据 TCM 起始地址 */

#define BOARD_PSRAM_BASE       0x60000000              /* 外挂 PSRAM 起始地址 */
#define BOARD_PSRAM_SIZE       (16 * 1024 * 1024)      /* 外挂 PSRAM 大小：16MB */

#define BOARD_FLASH_BASE       0x02000000              /* Flash XIP 起始地址 */
#define BOARD_FLASH_SIZE       (8 * 1024 * 1024)       /* Flash 总大小：8MB */

/* ═══════════════════════════════════════════════════════════════════════════
 * 串口引脚定义
 *
 * R1 板子上，UART0（下载/调试串口）通过 CH340 芯片连接到 USB：
 *   - BK7258 GPIO11 → UART0_TX（发送）
 *   - BK7258 GPIO10 → UART0_RX（接收）
 *
 * 这些只是文档性质的宏，实际引脚配置在芯片层的 bk7258_lowputc.c 中完成。
 * ═══════════════════════════════════════════════════════════════════════════ */

#define BOARD_UART0_TX_GPIO    11    /* UART0 发送引脚：GPIO11 */
#define BOARD_UART0_RX_GPIO    10    /* UART0 接收引脚：GPIO10 */

/* ═══════════════════════════════════════════════════════════════════════════
 * LED 状态指示（M1 阶段全部禁用）
 *
 * NuttX 内核在启动的关键阶段会调用 led_* 函数来指示系统状态。
 * 但 R1 板子还没有分配状态 LED，所以全部设为 0（无效）。
 *
 * 后续阶段如果分配了 LED，可以在这里定义：
 *   #define LED_STARTED     (GPIO_OUTPUT | GPIO_VALUE_ONE | GPIO_PORTn | GPIO_PINn)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LED_STARTED            0   /* M1: 禁用 */
#define LED_HEAPALLOCATE       0   /* M1: 禁用 */
#define LED_IRQSENABLED        0   /* M1: 禁用 */
#define LED_STACKCREATED       0   /* M1: 禁用 */
#define LED_INIRQ              0   /* M1: 禁用 */
#define LED_SIGNAL             0   /* M1: 禁用 */
#define LED_ASSERTION          0   /* M1: 禁用 */
#define LED_PANIC              0   /* M1: 禁用 */

/* ═══════════════════════════════════════════════════════════════════════════
 * 板级函数声明
 *
 * arm_boardinitialize() 在启动早期被 __start/bk7258_cstart 调用，
 * 用于做一些必须在调度器启动前完成的板级初始化（如 GPIO、时钟等）。
 * 目前是空函数，实现在 src/bk7258_boardinitialize.c 中。
 * ═══════════════════════════════════════════════════════════════════════════ */

void arm_boardinitialize(void);

#endif /* __BOARDS_BK7258_R1_INCLUDE_BOARD_H */
