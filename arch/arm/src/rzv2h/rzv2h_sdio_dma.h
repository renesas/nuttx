/****************************************************************************
 * arch/arm/src/rzv2h/rzv2h_sdio_dma.h
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

#ifndef __ARCH_ARM_SRC_RZV2H_RZV2H_SDIO_DMA_H
#define __ARCH_ARM_SRC_RZV2H_RZV2H_SDIO_DMA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Internal state, serialized by the parent host's IRQ/critical section.
 * Counters describe actual starts/completions, not selected configuration.
 */

struct rzv2h_sdiodma_s
{
  uintptr_t base;
  uintptr_t cpu_address;         /* CPU buffer or private staging range */
  uint32_t address;              /* Address presented to the bus master */
  uint32_t info1;
  uint32_t info2;
  uint32_t starts;
  uint32_t completions;
  uint32_t errors;
  uint32_t fallbacks;
#ifdef CONFIG_RZV2H_SDIO_DMA_BOUNCE
  const void *bounce_buffer;
  size_t bounce_length;
  uint32_t bounce_starts;
  uint32_t bounce_completions;
  bool bounced;
#endif
#ifdef CONFIG_ARCH_DCACHE
  size_t cache_length;            /* Owned whole-line DMA range */
  bool cache_active;              /* Cache work pending until quiescence */
#endif
  bool prepared;
  bool running;
  bool read;
  bool done;
  bool access;
  bool reset_used;
  uint16_t stop_offset;           /* Reset register on failed stop */
  uint32_t stop_value;            /* Failed reset readback */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int rzv2h_sdiodma_prepare(struct rzv2h_sdiodma_s *dma, uintptr_t base,
                         const void *buffer, size_t length,
                         unsigned int blocklen, bool read);
int rzv2h_sdiodma_finish(struct rzv2h_sdiodma_s *dma, int result);
void rzv2h_sdiodma_start(struct rzv2h_sdiodma_s *dma);
int rzv2h_sdiodma_poll(struct rzv2h_sdiodma_s *dma, bool access);
int rzv2h_sdiodma_stop(struct rzv2h_sdiodma_s *dma);
void rzv2h_sdiodma_initialize(struct rzv2h_sdiodma_s *dma, uintptr_t base);

#endif /* __ARCH_ARM_SRC_RZV2H_RZV2H_SDIO_DMA_H */
