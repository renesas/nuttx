/****************************************************************************
 * arch/arm/src/rzv2h/hardware/rzv2h_pinmap.h
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

#ifndef __ARCH_ARM_SRC_RZV2H_HARDWARE_RZV2H_PINMAP_H
#define __ARCH_ARM_SRC_RZV2H_HARDWARE_RZV2H_PINMAP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "bsp_api.h"
#include "r_ioport.h"
#include "chip.h"
#include "r_ioport.h"

/****************************************************************************
 * Porting implementation
 ****************************************************************************/

/* Pin configuration encoding. */

#define RZV2H_PINMUX(port, pin, mode) \
  ((gpio_pinset_t) \
    { \
      (uint32_t)(((port) << 8) | (pin)), \
      IOPORT_CFG_DRIVE_B01 | IOPORT_CFG_PERIPHERAL_PIN | \
      IOPORT_CFG_SLEW_RATE_FAST | ((uint32_t)(mode) << 24) \
    })

/* SCI-B UART pin configurations. */

/* SCI-B channel 0. */

#define GPIO_TXD0_MOSI0_DA0_P5_0_M1   RZV2H_PINMUX(5, 0, 1)
#define GPIO_RXD0_MISO0_SCL0_P5_1_M1  RZV2H_PINMUX(5, 1, 1)

/* SCI-B channel 1. */

#define GPIO_TXD1_MOSI1_SDA1_P5_2_M1  RZV2H_PINMUX(5, 2, 1)
#define GPIO_RXD1_MISO1_SCL1_P5_3_M1  RZV2H_PINMUX(5, 3, 1)

/* SCI-B channel 2. */

#define GPIO_TXD2_MOSI2_SDA2_P5_4_M1  RZV2H_PINMUX(5, 4, 1)
#define GPIO_TXD2_MOSI2_SDA2_P6_0_M6  RZV2H_PINMUX(6, 0, 6)
#define GPIO_RXD2_MISO2_SCL2_P5_5_M1  RZV2H_PINMUX(5, 5, 1)
#define GPIO_RXD2_MISO2_SCL2_P6_1_M6  RZV2H_PINMUX(6, 1, 6)

/* SCI-B channel 3. */

#define GPIO_TXD3_MOSI3_SDA3_P5_6_M1  RZV2H_PINMUX(5, 6, 1)
#define GPIO_TXD3_MOSI3_SDA3_P6_2_M6  RZV2H_PINMUX(6, 2, 6)
#define GPIO_RXD3_MISO3_SCL3_P5_7_M1  RZV2H_PINMUX(5, 7, 1)
#define GPIO_RXD3_MISO3_SCL3_P6_3_M6  RZV2H_PINMUX(6, 3, 6)

/* SCI-B channel 4. */

#define GPIO_TXD4_MOSI1_SDA4_P4_0_M2  RZV2H_PINMUX(4, 0, 2)
#define GPIO_TXD4_MOSI4_SDA4_P6_4_M6  RZV2H_PINMUX(6, 4, 6)
#define GPIO_TXD4_MOSI4_SDA4_P7_0_M1  RZV2H_PINMUX(7, 0, 1)
#define GPIO_TXD4_MOSI4_SDA4_P8_4_M6  RZV2H_PINMUX(8, 4, 6)
#define GPIO_TXD4_MOSI8_SDA8_P8_0_M6  RZV2H_PINMUX(8, 0, 6)
#define GPIO_TXD4_MOSI9_SDA0_P9_2_M6  RZV2H_PINMUX(9, 2, 6)
#define GPIO_RXD4_MISO4_SCL4_P4_1_M2  RZV2H_PINMUX(4, 1, 2)
#define GPIO_RXD4_MISO4_SCL4_P7_1_M1  RZV2H_PINMUX(7, 1, 1)
#define GPIO_RXD4_MISO4_SCL4_P8_5_M6  RZV2H_PINMUX(8, 5, 6)
#define GPIO_RXD4_MISO6_SCL6_P6_5_M6  RZV2H_PINMUX(6, 5, 6)

/* SCI-B channel 5. */

#define GPIO_TXD5_MOSI5_SDA5_P4_4_M2  RZV2H_PINMUX(4, 4, 2)
#define GPIO_TXD5_MOSI5_SDA5_P7_2_M1  RZV2H_PINMUX(7, 2, 1)
#define GPIO_TXD5_MOSI5_SDA5_P8_6_M6  RZV2H_PINMUX(8, 6, 6)
#define GPIO_RXD5_MISO5_SCL5_P4_5_M2  RZV2H_PINMUX(4, 5, 2)
#define GPIO_RXD5_MISO5_SCL5_P7_3_M1  RZV2H_PINMUX(7, 3, 1)
#define GPIO_RXD5_MISO5_SCL5_P8_7_M6  RZV2H_PINMUX(8, 7, 6)

/* SCI-B channel 6. */

#define GPIO_TXD6_MOSI6_SDA6_P7_4_M1  RZV2H_PINMUX(7, 4, 1)
#define GPIO_RXD6_MISO6_SCL6_P7_5_M1  RZV2H_PINMUX(7, 5, 1)

/* SCI-B channel 7. */

#define GPIO_TXD7_MOSI7_SDA7_P6_6_M6  RZV2H_PINMUX(6, 6, 6)
#define GPIO_TXD7_MOSI7_SDA7_P7_6_M1  RZV2H_PINMUX(7, 6, 1)
#define GPIO_TXD7_MOSI7_SDA7_P9_4_M2  RZV2H_PINMUX(9, 4, 2)
#define GPIO_RXD7_MISO7_SCL7_P6_7_M6  RZV2H_PINMUX(6, 7, 6)
#define GPIO_RXD7_MISO7_SCL7_P7_7_M1  RZV2H_PINMUX(7, 7, 1)
#define GPIO_RXD7_MISO7_SCL7_P9_5_M2  RZV2H_PINMUX(9, 5, 2)

/* SCI-B channel 8. */

#define GPIO_TXD8_MOSI8_SDA6_P9_0_M2  RZV2H_PINMUX(9, 0, 2)
#define GPIO_TXD8_MOSI8_SDA8_PB_1_M2  RZV2H_PINMUX(11, 1, 2)
#define GPIO_RXD8_MISO8_SCL6_P9_1_M2  RZV2H_PINMUX(9, 1, 2)
#define GPIO_RXD8_MISO8_SCL8_P8_1_M6  RZV2H_PINMUX(8, 1, 6)
#define GPIO_RXD8_MISO8_SCL8_PB_2_M2  RZV2H_PINMUX(11, 2, 2)

/* SCI-B channel 9. */

#define GPIO_TXD9_MOSI9_SDA9_P8_2_M6  RZV2H_PINMUX(8, 2, 6)
#define GPIO_TXD9_MOSI9_SDA9_PB_4_M2  RZV2H_PINMUX(11, 4, 2)
#define GPIO_RXD9_MISO0_SCL0_P9_3_M6  RZV2H_PINMUX(9, 3, 6)
#define GPIO_RXD9_MISO9_SCL9_P8_3_M6  RZV2H_PINMUX(8, 3, 6)
#define GPIO_RXD9_MISO9_SCL9_PB_3_M2  RZV2H_PINMUX(11, 3, 2)

#endif /* __ARCH_ARM_SRC_RZV2H_HARDWARE_RZV2H_PINMAP_H */
