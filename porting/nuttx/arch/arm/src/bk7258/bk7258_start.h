/****************************************************************************
 * arch/arm/src/bk7258/bk7258_start.h
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
 * 文件角色：启动入口声明（__start 函数声明）
 *
 * 声明 __start() 函数，这是整个 bring-up 流程的汇编入口点。
 * 定义在 bk7258_start.c 中，被 arm_vectors.c 的向量表 _vectors[1] 引用。
 *
 * 通俗理解：向量表的 _vectors[1] 位置需要填一个函数地址，这个文件就是
 * 告诉编译器"__start 函数存在，它的实现在 bk7258_start.c 里"。
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_BK7258_BK7258_START_H
#define __ARCH_ARM_SRC_BK7258_BK7258_START_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Reset entry point (defined in bk7258_start.c) */

void __start(void);

#endif /* __ARCH_ARM_SRC_BK7258_BK7258_START_H */
