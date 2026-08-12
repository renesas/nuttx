/****************************************************************************
 * arch/arm/src/rzv2h/rzv2h_irq.h
 *
 * SPDX-License-Identifier: Apache-2.0
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

#ifndef __ARCH_ARM_SRC_RZV2H_RZV2H_IRQ_H
#define __ARCH_ARM_SRC_RZV2H_RZV2H_IRQ_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "bsp_api.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* FSP numbers peripheral interrupts relative to the first SPI.  NuttX uses
 * the absolute GIC interrupt ID, including the 16 SGIs and 16 PPIs.
 */

#define RZV2H_FSP_TO_GIC_IRQ(irq) \
  ((int)(irq) + BSP_CORTEX_VECTOR_TABLE_ENTRIES)

#endif /* __ARCH_ARM_SRC_RZV2H_RZV2H_IRQ_H */
