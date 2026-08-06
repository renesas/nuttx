/****************************************************************************
 * arch/arm/src/rzv2h/rzv2h_irq.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <sched.h>
#include "arm_internal.h"
#include "sctlr.h"
#include "gic.h"
/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_irqinitialize
 ****************************************************************************/

void up_irqinitialize(void)
{
  /* Initialize the Generic Interrupt Controller (GIC) for CPU0.
   * In AMP mode, we want arm_gic0_initialize to be called only once.
   */

  if (sched_getcpu() == 0)
    {
      arm_gic0_initialize();  /* Initialization unique to CPU0 */
    }

  arm_gic_initialize();   /* Initialization common to all CPUs */

#ifndef CONFIG_SUPPRESS_INTERRUPTS
  /* And finally, enable interrupts */

  arm_color_intstack();
  up_irq_enable();
#endif
}
