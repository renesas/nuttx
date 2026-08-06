/****************************************************************************
 * boards/arm/rzv2h/rzv2h-evk/include/board.h
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

#ifndef __BOARDS_ARM_RZV2H_RZV2H_EVK_INCLUDE_BOARD_H
#define __BOARDS_ARM_RZV2H_RZV2H_EVK_INCLUDE_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* LED definitions **********************************************************/

/* If CONFIG_ARCH_LEDS is defined, the LEDs will be controlled as follows
 * for NuttX debug functionality (where NC means "No Change").
 */

#define LED_STARTED       0  /* OFF      OFF      OFF      OFF      */
#define LED_HEAPALLOCATE  1  /* ON       OFF      OFF      OFF      */
#define LED_IRQSENABLED   2  /* OFF      ON       OFF      OFF      */
#define LED_STACKCREATED  3  /* OFF      OFF      ON       OFF      */
#define LED_INIRQ         4  /* NC       NC       NC       ON       */
#define LED_SIGNAL        5  /* NC       NC       NC       NC       */
#define LED_ASSERTION     6  /* BLINK    NC       NC       NC       */
#define LED_PANIC         7  /* BLINK    BLINK    BLINK    BLINK    */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: rzv2h_board_initialize
 *
 * Description:
 *   All RZV2H architectures must provide the following entry point. This
 *   entry point is called early in the initialization -- after clocking
 *   and memory have been configured but before caches have been enabled
 *   and before any devices have been initialized.
 *
 ****************************************************************************/

void rzv2h_board_initialize(void);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __BOARDS_ARM_RZV2H_RZV2H_EVK_INCLUDE_BOARD_H */
