/****************************************************************************
 * arch/arm/src/bk7258/bk7258_irq.c
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
 * 文件角色：中断系统（NVIC + BK7258 特有的 SYS 级中断聚合器）
 *
 * bring-up 流程位置：nx_start() → up_irqinitialize() → 本文件
 *
 * ⚠️ BK7258 最特殊的地方：两级中断使能
 * ─────────────────────────────────────────────────
 * 普通芯片：外设中断 → NVIC 开关 → CPU，只需要一步。
 * BK7258：  外设中断 → SYS 级聚合器 → NVIC 开关 → CPU，需要两步。
 *
 *   ┌──────────┐      ┌─────────────────┐      ┌──────┐      ┌─────┐
 *   │ 外设中断  │ ───→ │ SYS 聚合器       │ ───→ │ NVIC │ ───→ │ CPU │
 *   │ (UART等) │      │ 0x44010080/84    │      │      │      │ CPU0│
 *   └──────────┘      └─────────────────┘      └──────┘      └─────┘
 *                    ↑ 这是 BK7258 特有的   ↑ 这是 Arm 标准的
 *
 * 实测发现：UART RX 中断在外设侧已经触发，但因为没有在 SYS 级使能，
 * 中断永远到不了 NVIC → NSH 命令行完全没反应。
 *
 * 核心函数：
 *   up_irqinitialize()  → 初始化中断系统（nx_start() 调用）
 *   up_enable_irq()     → 使能中断：SYS 级 + NVIC 级，两步都要做
 *   up_disable_irq()    → 禁用中断：SYS 级 + NVIC 级，两步都要做
 *   bk7258_fault_spin() → SWD 调试辅助：故障时原地死循环，方便调试器查看
 *
 * 参考：arch/arm/src/mps/mps_irq.c（Armv8-M NVIC 通用模板）
 *       arch/arm/include/bk7258/irq.h（中断编号定义）
 ****************************************************************************/

/* Porting reference: arch/arm/src/mps/mps_irq.c (generic armv8-m NVIC). */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>

#include "ram_vectors.h"
#include "arm_internal.h"
#include "nvic.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* BK7258_IRQ_FIRST：第一个外设中断的编号。
 * 0-15 是系统异常（NMI, HardFault, SVCall, SysTick 等），
 * 16（BK7258_IRQ_EXTINT）开始是外设中断。 */

#define BK7258_IRQ_FIRST   BK7258_IRQ_EXTINT

/* NVIC 使能/禁用寄存器的偏移量关系：
 * NVIC_IRQ0_31_ENABLE(0xE000E100) 和 NVIC_IRQ0_31_CLEAR(0xE000E180) 相差 0x80。
 * 通过这个偏移量，同一个 irqinfo 函数可以找到使能或禁用寄存器。 */

#define NVIC_ENA_OFFSET    (0)
#define NVIC_CLRENA_OFFSET (NVIC_IRQ0_31_CLEAR - NVIC_IRQ0_31_ENABLE)

/* DEFPRIORITY32：一个 32 位字里打包了 4 个中断优先级，每个 8 位，全部设为默认值。
 * NVIC 优先级寄存器是 4 个中断一组（每个 8 位），这样一次写入 32 位就设了 4 个。 */

#define DEFPRIORITY32      (NVIC_SYSH_PRIORITY_DEFAULT << 24 | \
                            NVIC_SYSH_PRIORITY_DEFAULT << 16 | \
                            NVIC_SYSH_PRIORITY_DEFAULT << 8  | \
                            NVIC_SYSH_PRIORITY_DEFAULT)

/* ═══════════════════════════════════════════════════════════════════════════
 * BK7258 特有的 SYS 级中断聚合器
 *
 * 除了标准的 NVIC 外，BK7258 还有一层额外的中断开关。外设中断要到达 CPU0
 * 的 NVIC，必须先在 SYS 级使能。每个 bit 对应一个中断源（编号与 NVIC 一致）：
 *   - 中断 0-31：  0x44010080 寄存器
 *   - 中断 32-63： 0x44010084 寄存器
 *
 * 实测验证：UART RX 中断在外设侧已经触发，但因为没在 SYS 级使能，
 * 中断永远到不了 NVIC → NSH 命令行完全没反应。
 * ═══════════════════════════════════════════════════════════════════════════ */

#define BK7258_SYS_CPU0_INT_0_31_EN   0x44010080
#define BK7258_SYS_CPU0_INT_32_63_EN  0x44010084

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_BK7258_DEBUG_FAULT_SPIN

/* ═══════════════════════════════════════════════════════════════════════════
 * SWD 调试辅助：故障时原地死循环，方便调试器查看现场
 *
 * 正常流程：发生故障 → 调 arm_hardfault() → 尝试打印错误信息 → 可能再次故障
 *    → 调试器连接时看不到完整的故障现场（栈帧被破坏了）
 *
 * 调试流程：发生故障 → 调 bk7258_fault_spin() → 原地死循环
 *    → 用 SWD 调试器连接 → 读 PC/LR/CFSR 寄存器 → 知道是哪里出了问题
 *
 * g_bk7258_fault_regs：保存故障时的寄存器帧指针，OpenOCD/gdb 可以直接读。
 * ═══════════════════════════════════════════════════════════════════════════ */

volatile void *g_bk7258_fault_regs;

static int bk7258_fault_spin(int irq, void *context, void *arg)
{
#ifdef CONFIG_BK7258_DEBUG_FAULT_SPIN_MARK
  const char *s = "\r\n!!BKFAULT!! irq=";
#endif

  g_bk7258_fault_regs = context;    /* 保存寄存器帧，调试器可以读 */

  up_irq_save();                     /* 关全局中断，防止其他中断干扰 */

#ifdef CONFIG_BK7258_DEBUG_FAULT_SPIN_MARK
  /* 尝试输出故障标记（用轮询模式，不依赖中断），帮助定位哪个异常 */
  while (*s)
    {
      arm_lowputc(*s++);
    }

  /* 输出异常编号（只输出个位数：HardFault=3, MemFault=4, BusFault=5, UsageFault=6） */
  arm_lowputc('0' + (irq % 10));
  arm_lowputc('\r');
  arm_lowputc('\n');
#endif

  /* 原地死循环：CPU 停在这里，MSP 上的异常帧完整保留，
   * SWD 调试器连接后可以直接读 PC、CFSR 等寄存器定位故障原因。 */
  for (; ; )
    {
    }

  return 0;
}
#endif /* CONFIG_BK7258_DEBUG_FAULT_SPIN */

#ifdef CONFIG_DEBUG_FEATURES
/* ── bk7258_nmi ────────────────────────────────────────────────────────────
 * NMI（不可屏蔽中断）处理：通常是硬件严重错误，直接 PANIC。
 * 正常情况下 NMI 不应该触发，触发了说明有硬件问题。 */

static int bk7258_nmi(int irq, void *context, void *arg)
{
  up_irq_save();
  _err("PANIC!!! NMI received\n");
  PANIC();
  return 0;
}

/* ── bk7258_pendsv ─────────────────────────────────────────────────────────
 * PendSV（可挂起系统调用）处理：正常情况下由 NuttX 内核管理，用于任务切换。
 * 如果意外触发（比如 PendSV 被软件错误触发），直接 PANIC。 */

static int bk7258_pendsv(int irq, void *context, void *arg)
{
  up_irq_save();
  _err("PANIC!!! PendSV received\n");
  PANIC();
  return 0;
}

/* ── bk7258_reserved ───────────────────────────────────────────────────────
 * 保留中断处理：如果触发了没有分配的中断号，说明系统出了问题。 */

static int bk7258_reserved(int irq, void *context, void *arg)
{
  up_irq_save();
  _err("PANIC!!! Reserved interrupt\n");
  PANIC();
  return 0;
}
#endif

/* ── bk7258_prioritize_syscall ─────────────────────────────────────────────
 * 设置 SVCall（系统调用）中断的优先级。
 * SVCall 是系统异常 11，优先级寄存器在 NVIC_SYSH8_11_PRIORITY 的高 8 位。 */

static inline void bk7258_prioritize_syscall(int priority)
{
  uint32_t regval;

  /* 读 → 清 SVCall 优先级位 → 写新值 → 写回 */
  regval  = getreg32(NVIC_SYSH8_11_PRIORITY);
  regval &= ~NVIC_SYSH_PRIORITY_PR11_MASK;
  regval |= (priority << NVIC_SYSH_PRIORITY_PR11_SHIFT);
  putreg32(regval, NVIC_SYSH8_11_PRIORITY);
}

/* ── bk7258_irqinfo ────────────────────────────────────────────────────────
 * 根据中断号找到对应的 NVIC 寄存器地址和 bit 位。
 *
 * 输入：irq = 中断号, offset = 偏移量（ENABLE 或 CLEAR 寄存器）
 * 输出：regaddr = 寄存器地址, bit = 对应的 bit 位
 *
 * 对于外设中断（irq >= 16）：找 NVIC 使能/禁用寄存器
 * 对于系统异常（irq < 16）：MemFault/BusFault/UsageFault 在 NVIC_SYSHCON 中，
 *   SysTick 在 NVIC_SYSTICK_CTRL 中
 * ─────────────────────────────────────────────────────────────────────────── */

static int bk7258_irqinfo(int irq, uintptr_t *regaddr, uint32_t *bit,
                          uintptr_t offset)
{
  int n;

  DEBUGASSERT(irq >= BK7258_IRQ_NMI && irq < NR_IRQS);

  if (irq >= BK7258_IRQ_FIRST)     /* 外设中断（16-79） */
    {
      n        = irq - BK7258_IRQ_FIRST;           /* 外设中断索引（0-63） */
      *regaddr = NVIC_IRQ_ENABLE(n) + offset;       /* 使能/禁用寄存器地址 */
      *bit     = (uint32_t)0x1 << (n & 0x1f);        /* 32 个一组，取余 32 */
    }
  else                             /* 系统异常（0-15） */
    {
      *regaddr = NVIC_SYSHCON;
      if (irq == BK7258_IRQ_MEMFAULT)
        {
          *bit = NVIC_SYSHCON_MEMFAULTENA;           /* MemFault 使能位 */
        }
      else if (irq == BK7258_IRQ_BUSFAULT)
        {
          *bit = NVIC_SYSHCON_BUSFAULTENA;           /* BusFault 使能位 */
        }
      else if (irq == BK7258_IRQ_USAGEFAULT)
        {
          *bit = NVIC_SYSHCON_USGFAULTENA;           /* UsageFault 使能位 */
        }
      else if (irq == BK7258_IRQ_SYSTICK)
        {
          *regaddr = NVIC_SYSTICK_CTRL;              /* SysTick 控制寄存器 */
          *bit = NVIC_SYSTICK_CTRL_ENABLE;           /* SysTick 使能位 */
        }
      else
        {
          return -EINVAL;                            /* 不支持的中断号 */
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* ── up_prioritize_irq ─────────────────────────────────────────────────────
 * 设置某个中断的优先级。
 *
 * NVIC 优先级寄存器是 4 个中断一组（每个 8 位），所以要：
 *   1. 找到这个中断在哪个优先级寄存器里
 *   2. 找到在 32 位寄存器里的偏移（8 位一组）
 *   3. 读 → 清旧值 → 写新值 → 写回
 *
 * 中断号 0-15（系统异常）和 16+（外设中断）用不同的寄存器组。 */

int up_prioritize_irq(int irq, int priority)
{
  uint32_t regaddr;
  uint32_t regval;
  int shift;

  DEBUGASSERT(irq >= 0 && irq < NR_IRQS &&
              (unsigned)priority <= NVIC_SYSH_PRIORITY_MIN);

  if (irq < 16)                         /* 系统异常（0-15） */
    {
      regaddr = NVIC_SYSH_PRIORITY(irq);
      irq    -= 4;                      /* 偏移调整 */
    }
  else                                  /* 外设中断（16+） */
    {
      irq    -= 16;                     /* 外设中断从 0 开始编号 */
      regaddr = NVIC_IRQ_PRIORITY(irq);
    }

  regval  = getreg32(regaddr);          /* 读当前优先级寄存器 */
  shift   = ((irq & 3) << 3);           /* 4 个一组，每个 8 位，算偏移 */
  regval &= ~(0xff << shift);           /* 清掉旧优先级 */
  regval |= (priority << shift);        /* 写入新优先级 */
  putreg32(regval, regaddr);            /* 写回 */

  return OK;
}

/* ── up_irqinitialize ──────────────────────────────────────────────────────
 * 中断系统初始化（nx_start() 调用）。
 *
 * 这是整个中断系统启动的入口，做了以下事情：
 *   1. 禁用所有外设中断（防止 bootloader 留的旧中断干扰）
 *   2. 设置 VTOR 指向我们的向量表
 *   3. 设置所有中断优先级为默认值
 *   4. 注册 SVCall、HardFault 等核心异常处理函数
 *   5. 可选：启用故障死循环调试模式
 *   6. 开全局中断
 *
 * 执行顺序：先清干净旧状态 → 配好优先级 → 注册处理函数 → 最后开中断。 */

void up_irqinitialize(void)
{
  uint32_t regaddr;
  int num_priority_registers;
  int i;

  /* 步骤 1：禁用所有外设中断。
   * 写 NVIC_IRQ_CLEAR 寄存器，bit 置 1 表示禁用对应中断。
   * 每次写 32 位（32 个中断一组），循环覆盖所有外设中断。 */

  for (i = 0; i < NR_IRQS - BK7258_IRQ_FIRST; i += 32)
    {
      putreg32(0xffffffff, NVIC_IRQ_CLEAR(i));  /* 禁用 32 个中断 */
    }

  /* 步骤 2：设置 VTOR（向量表偏移寄存器），指向我们的向量表 */

  putreg32((uint32_t)_vectors, NVIC_VECTAB);

#ifdef CONFIG_ARCH_RAMVECTORS
  arm_ramvec_initialize();             /* 如果启用 RAM 向量表，初始化它 */
#endif

  /* 步骤 3：设置所有中断（系统异常 + 外设中断）的优先级为默认值。
   * 系统异常优先级寄存器：NVIC_SYSH4_7 / 8_11 / 12_15（每组 4 个） */

  putreg32(DEFPRIORITY32, NVIC_SYSH4_7_PRIORITY);
  putreg32(DEFPRIORITY32, NVIC_SYSH8_11_PRIORITY);
  putreg32(DEFPRIORITY32, NVIC_SYSH12_15_PRIORITY);

  /* 外设中断优先级寄存器：读 ICTR 获知有多少个寄存器，逐个写默认值 */

  num_priority_registers = (getreg32(NVIC_ICTR) + 1) * 8;

  regaddr = NVIC_IRQ0_3_PRIORITY;
  while (num_priority_registers--)
    {
      putreg32(DEFPRIORITY32, regaddr);
      regaddr += 4;                     /* 下一个 32 位优先级寄存器 */
    }

  /* 步骤 4：注册核心异常处理函数。
   * SVCall 用于 NuttX 系统调用，HardFault 用于处理硬件错误。 */

  irq_attach(BK7258_IRQ_SVCALL, arm_svcall, NULL);
  irq_attach(BK7258_IRQ_HARDFAULT, arm_hardfault, NULL);

  /* 设置 PendSV 为最低优先级（用于任务切换），SVCall 为指定优先级 */

  up_prioritize_irq(BK7258_IRQ_PENDSV, NVIC_SYSH_PRIORITY_MIN);
  bk7258_prioritize_syscall(NVIC_SYSH_SVCALL_PRIORITY);

  /* 步骤 5：使能 MemFault（如果启用 MPU） */

#ifdef CONFIG_ARM_MPU
  irq_attach(BK7258_IRQ_MEMFAULT, arm_memfault, NULL);
  up_enable_irq(BK7258_IRQ_MEMFAULT);
#endif

  /* 步骤 6：调试模式下注册额外的异常处理 */

#ifdef CONFIG_DEBUG_FEATURES
  irq_attach(BK7258_IRQ_NMI, bk7258_nmi, NULL);
#ifndef CONFIG_ARM_MPU
  irq_attach(BK7258_IRQ_MEMFAULT, arm_memfault, NULL);
#endif
  irq_attach(BK7258_IRQ_BUSFAULT, arm_busfault, NULL);
  irq_attach(BK7258_IRQ_USAGEFAULT, arm_usagefault, NULL);
  irq_attach(BK7258_IRQ_PENDSV, bk7258_pendsv, NULL);
  irq_attach(BK7258_IRQ_DBGMONITOR, arm_dbgmonitor, NULL);
  irq_attach(BK7258_IRQ_RESERVED, bk7258_reserved, NULL);
#endif

  /* 步骤 7：SWD 调试模式：用故障死循环 handler 覆盖默认 handler。
   * 同时分别使能 MemFault/BusFault/UsageFault，防止它们升级为 HardFault
   *  → OpenOCD 可以区分是哪种故障（通过 CFSR 寄存器）。 */

#ifdef CONFIG_BK7258_DEBUG_FAULT_SPIN
  irq_attach(BK7258_IRQ_HARDFAULT, bk7258_fault_spin, NULL);
  irq_attach(BK7258_IRQ_MEMFAULT, bk7258_fault_spin, NULL);
  irq_attach(BK7258_IRQ_BUSFAULT, bk7258_fault_spin, NULL);
  irq_attach(BK7258_IRQ_USAGEFAULT, bk7258_fault_spin, NULL);
  up_enable_irq(BK7258_IRQ_MEMFAULT);
  up_enable_irq(BK7258_IRQ_BUSFAULT);
  up_enable_irq(BK7258_IRQ_USAGEFAULT);
#endif

  /* 步骤 8：开全局中断！之前一直关着，现在一切就绪，可以响应中断了。 */

#ifndef CONFIG_SUPPRESS_INTERRUPTS
  arm_color_intstack();                /* 给中断栈上色（调试用，检测栈溢出） */
  up_irq_enable();                     /* 全局开中断（cpsie i） */
#endif
}

/* ── bk7258_sys_int_set ────────────────────────────────────────────────────
 * BK7258 特有的 SYS 级中断开关（两级中断的第二级）。
 *
 * enable=true  → 在 SYS 聚合器寄存器中置位 → 中断能到达 NVIC
 * enable=false → 在 SYS 聚合器寄存器中清零 → 中断被 SYS 级挡住
 *
 * 中断源 0-31 在 0x44010080，中断源 32-63 在 0x44010084。 */

static void bk7258_sys_int_set(int irq, bool enable)
{
  uintptr_t reg;
  uint32_t bit;
  int src;

  if (irq < BK7258_IRQ_EXTINT)         /* 系统异常不经过 SYS 聚合器 */
    {
      return;
    }

  src = irq - BK7258_IRQ_EXTINT;       /* 中断源编号（0-63） */
  if (src < 32)                         /* 中断 0-31 */
    {
      reg = BK7258_SYS_CPU0_INT_0_31_EN;
      bit = 1u << src;
    }
  else                                  /* 中断 32-63 */
    {
      reg = BK7258_SYS_CPU0_INT_32_63_EN;
      bit = 1u << (src - 32);
    }

  if (enable)                           /* 读 → 置位 → 写回 */
    {
      putreg32(getreg32(reg) | bit, reg);
    }
  else                                  /* 读 → 清零 → 写回 */
    {
      putreg32(getreg32(reg) & ~bit, reg);
    }
}

/* ── up_disable_irq ────────────────────────────────────────────────────────
 * 禁用某个中断。
 *
 * 两步操作：
 *   1. NVIC 级禁用（写 NVIC_IRQ_CLEAR 寄存器）
 *   2. SYS 聚合器级禁用（bk7258_sys_int_set(false)）
 * 两步都必须做，否则中断可能还是能到达 CPU。 */

void up_disable_irq(int irq)
{
  uintptr_t regaddr;
  uint32_t regval;
  uint32_t bit;

  if (bk7258_irqinfo(irq, &regaddr, &bit, NVIC_CLRENA_OFFSET) == 0)
    {
      if (irq >= BK7258_IRQ_FIRST)     /* 外设中断：NVIC + SYS 两级都禁用 */
        {
          putreg32(bit, regaddr);       /* NVIC 级：写 CLEAR 寄存器禁用 */
          bk7258_sys_int_set(irq, false); /* SYS 级：清聚合器对应位 */
        }
      else                              /* 系统异常：只操作 NVIC（无 SYS 级） */
        {
          regval  = getreg32(regaddr);
          regval &= ~bit;
          putreg32(regval, regaddr);
        }
    }
}

/* ── up_enable_irq ─────────────────────────────────────────────────────────
 * 使能某个中断。
 *
 * 两步操作（与 disable 对称）：
 *   1. NVIC 级使能（写 NVIC_IRQ_ENABLE 寄存器）
 *   2. SYS 聚合器级使能（bk7258_sys_int_set(true)）
 * ⚠️ 两步都必须做！忘记 SYS 级使能是最常见的 bug。 */

void up_enable_irq(int irq)
{
  uintptr_t regaddr;
  uint32_t regval;
  uint32_t bit;

  if (bk7258_irqinfo(irq, &regaddr, &bit, NVIC_ENA_OFFSET) == 0)
    {
      if (irq >= BK7258_IRQ_FIRST)     /* 外设中断：NVIC + SYS 两级都使能 */
        {
          putreg32(bit, regaddr);       /* NVIC 级：写 ENABLE 寄存器使能 */
          bk7258_sys_int_set(irq, true); /* SYS 级：置聚合器对应位 */
        }
      else                              /* 系统异常：只操作 NVIC（无 SYS 级） */
        {
          regval  = getreg32(regaddr);
          regval |= bit;
          putreg32(regval, regaddr);
        }
    }
}

/* ── arm_ack_irq ───────────────────────────────────────────────────────────
 * 中断应答：告诉中断控制器"我处理完了"。
 * BK7258 的 NVIC 不需要额外的应答操作（硬件自动处理），所以空函数。 */

void arm_ack_irq(int irq)
{
}
