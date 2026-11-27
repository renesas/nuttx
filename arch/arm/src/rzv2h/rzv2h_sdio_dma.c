/****************************************************************************
 * arch/arm/src/rzv2h/rzv2h_sdio_dma.c
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

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <debug.h>

#ifdef CONFIG_ARCH_DCACHE
#  include <nuttx/cache.h>
#endif

#include <arch/barriers.h>

#include "arm_internal.h"
#include "sctlr.h"
#include "hardware/rzv2h_sdhi.h"
#include "rzv2h_sdio_dma.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* R01UH1032EJ0140, system SRAM3 map and SDHI section 6.2.2.2.25-32.
 * Core0 DTCM local/AXI mapping follows the Renesas FSP CR8
 * bsp_slave_address.h. Local DTCM is non-cacheable in rzv2h_mpu_region.c.
 * Cached SRAM transfers require exclusively owned complete cache lines.
 */

/* Fixed SD-memory sector geometry required by this backend. */

#define SDIODMA_SECTOR_SIZE 512
#define SDIODMA_SECTOR_MASK (SDIODMA_SECTOR_SIZE - 1)

#define SDIODMA_SRAM_START  0x08180000u
#define SDIODMA_SRAM_END    0x08200000u
#define SDIODMA_DTCM_START  0x00020000u
#define SDIODMA_DTCM_END    0x00040000u
#define SDIODMA_DTCM_AXI    0x12060000u
#define SDIODMA_BOUNCE_ALIGNMENT   128u
#define SDIODMA_CACHE_LINE_32      32u
#define SDIODMA_CACHE_LINE_64      64u

#ifdef CONFIG_RZV2H_SDIO_DMA_BOUNCE
/* Each controller owns a bounded SRAM pool. The parent serializes setup,
 * completion and cancellation on this single-core backend. Separate pools
 * prevent cross-controller aliasing when both hosts have requests pending.
 */

#define SDIODMA_BOUNCE_SIZE 4096u

struct rzv2h_sdiodma_pool_s
{
  struct rzv2h_sdiodma_s *owner;
  uint8_t buffer[SDIODMA_BOUNCE_SIZE]
    __attribute__((aligned(SDIODMA_BOUNCE_ALIGNMENT)));
};

/* Keep identity metadata separate so zeroed staging stays in BSS. */

static const uintptr_t g_sdiodma_base[] =
{
#ifdef CONFIG_RZV2H_SDIO_CHANNEL0
  RZV2H_SDHI_BASE(0),
#endif
#ifdef CONFIG_RZV2H_SDIO_CHANNEL1
  RZV2H_SDHI_BASE(1),
#endif
};

#define SDIODMA_POOL_COUNT \
  (sizeof(g_sdiodma_base) / sizeof(g_sdiodma_base[0]))

static struct rzv2h_sdiodma_pool_s g_sdiodma_pool[SDIODMA_POOL_COUNT];
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Validate the whole CPU range before translating it for the bus master.
 * Subtraction avoids wrapping the range end. Do not accept CPU accesses
 * through the AXI alias: its MPU attributes differ from local DTCM.
 */

static bool rzv2h_sdiodma_map(uintptr_t address, size_t length,
                             uint32_t *bus, bool *cacheable)
{
  if (address >= SDIODMA_SRAM_START && address < SDIODMA_SRAM_END &&
      length <= SDIODMA_SRAM_END - address)
    {
      *bus = address;
      *cacheable = true;
      return true;
    }

#ifdef CONFIG_RZV2H_CORTEXR8_0
  if (address >= SDIODMA_DTCM_START && address < SDIODMA_DTCM_END &&
      length <= SDIODMA_DTCM_END - address)
    {
      *bus = address - SDIODMA_DTCM_START + SDIODMA_DTCM_AXI;
      *cacheable = false;
      return true;
    }
#endif

  return false;
}

#ifdef CONFIG_RZV2H_SDIO_DMA_BOUNCE
static struct rzv2h_sdiodma_pool_s *
rzv2h_sdiodma_pool(uintptr_t base)
{
  unsigned int channel;

  for (channel = 0; channel < SDIODMA_POOL_COUNT; channel++)
    {
      if (base == g_sdiodma_base[channel])
        {
          return &g_sdiodma_pool[channel];
        }
    }

  return NULL;
}
#endif

#ifdef CONFIG_ARCH_DCACHE
/* Called only after DMA completion or reset containment, before returning
 * ownership. The range is always complete, exclusively owned cache lines.
 */

static void rzv2h_sdiodma_cache_complete(struct rzv2h_sdiodma_s *dma)
{
  if (dma->cache_active)
    {
      if (dma->read)
        {
          up_invalidate_dcache(dma->cpu_address,
                               dma->cpu_address + dma->cache_length);
          UP_DSB();
        }

      dma->cache_active = false;
      dma->cache_length = 0;
    }
}
#endif

static void rzv2h_sdiodma_disable(struct rzv2h_sdiodma_s *dma)
{
  putreg32(UINT32_MAX, dma->base + RZV2H_SDHI_DM_CM_INFO1_MASK);
  putreg32(UINT32_MAX, dma->base + RZV2H_SDHI_DM_CM_INFO2_MASK);
  putreg16(SDHI_CC_EXT_MODE_RESET, dma->base + RZV2H_SDHI_CC_EXT_MODE);
  UP_DSB();
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void rzv2h_sdiodma_initialize(struct rzv2h_sdiodma_s *dma, uintptr_t base)
{
#ifdef CONFIG_RZV2H_SDIO_DMA_BOUNCE
  unsigned int channel;

  for (channel = 0; channel < SDIODMA_POOL_COUNT; channel++)
    {
      if (g_sdiodma_pool[channel].owner == dma)
        {
          g_sdiodma_pool[channel].owner = NULL;
        }
    }

  dma->bounced = false;
  dma->bounce_buffer = NULL;
  dma->bounce_length = 0;
#endif
  dma->base = base;
  rzv2h_sdiodma_disable(dma);

  /* Parent has already reset the host; no card communication is active. */

  putreg32(UINT32_MAX & ~SDHI_DM_RST_CHANNELS,
           base + RZV2H_SDHI_DM_CM_RST);
  getreg32(base + RZV2H_SDHI_DM_CM_RST);
  putreg32(UINT32_MAX, base + RZV2H_SDHI_DM_CM_RST);
  putreg32(0, base + RZV2H_SDHI_DM_CM_INFO1);
  putreg32(0, base + RZV2H_SDHI_DM_CM_INFO2);
  UP_DSB();
#ifdef CONFIG_ARCH_DCACHE
  /* Parent reset and the DMAC reset above have already quiesced the bus. */

  rzv2h_sdiodma_cache_complete(dma);
#endif
  dma->prepared = false;
  dma->running = false;
  dma->done = false;
  dma->access = false;
  dma->reset_used = false;
}

int rzv2h_sdiodma_prepare(struct rzv2h_sdiodma_s *dma, uintptr_t base,
                         const void *buffer, size_t length,
                         unsigned int blocklen, bool read)
{
  uintptr_t address = (uintptr_t)buffer;
  uintptr_t alignment = SDHI_DM_DTRAN_ADDR_ALIGNMENT;
  bool cache_enabled = (cp15_rdsctlr() & SCTLR_C) != 0;
  bool cacheable;
  bool cached;
  uint32_t bus;
  uint32_t endbit = read ? SDHI_DM_END_RX : SDHI_DM_END_TX;
  uint32_t errbit = read ? SDHI_DM_ERROR_RX : SDHI_DM_ERROR_TX;

  if (dma->prepared || dma->running
#ifdef CONFIG_RZV2H_SDIO_DMA_BOUNCE
      || dma->bounced
#endif
     )
    {
      return -EBUSY;
    }

  if (blocklen != SDIODMA_SECTOR_SIZE || length == 0 ||
      (length & SDIODMA_SECTOR_MASK) != 0 ||
      !rzv2h_sdiodma_map(address, length, &bus, &cacheable))
    {
      dma->fallbacks++;
      return 0;
    }

  cached = cache_enabled && cacheable;
#ifndef CONFIG_ARCH_DCACHE
  if (cached)
    {
      dma->fallbacks++;
      return 0;
    }
#endif

#ifdef CONFIG_ARCH_DCACHE
  if (cached)
    {
      /* Use complete cache API lines; unsupported granules stay on PIO. */

      alignment = up_get_dcache_linesize();
      if (alignment != SDIODMA_CACHE_LINE_32 &&
          alignment != SDIODMA_CACHE_LINE_64)
        {
          dma->fallbacks++;
          return 0;
        }
    }
#endif

  /* R01UH1032EJ0140 section 6.2.2.2.32, p1576: DADDR is bits31:3;
   * bits2:0 must be zero. Keep the staging pool separately 128-aligned.
   */

  if ((address & (alignment - 1)) != 0)
    {
#ifdef CONFIG_RZV2H_SDIO_DMA_BOUNCE
      struct rzv2h_sdiodma_pool_s *pool = rzv2h_sdiodma_pool(base);
      uintptr_t staging;

      if (pool == NULL)
        {
          dma->fallbacks++;
          return 0;
        }

      staging = (uintptr_t)pool->buffer;

      if (length > sizeof(pool->buffer) || pool->owner != NULL ||
          staging < SDIODMA_SRAM_START || staging >= SDIODMA_SRAM_END ||
          sizeof(pool->buffer) > SDIODMA_SRAM_END - staging)
        {
          dma->fallbacks++;
          return 0;
        }

      /* The staging pool is SRAM even when the caller is local DTCM.
       * Validate its cache policy before claiming ownership or copying.
       */

      cached = cache_enabled;
      if (cached)
        {
#ifdef CONFIG_ARCH_DCACHE
          alignment = up_get_dcache_linesize();
          if (alignment != SDIODMA_CACHE_LINE_32 &&
              alignment != SDIODMA_CACHE_LINE_64)
#endif
            {
              dma->fallbacks++;
              return 0;
            }
        }

      pool->owner = dma;
      dma->bounced = true;
      dma->bounce_buffer = buffer;
      dma->bounce_length = length;
      if (!read)
        {
          memcpy(pool->buffer, buffer, length);
        }

      address = staging;
      bus = staging;
#else
      dma->fallbacks++;
      return 0;
#endif
    }

  dma->base = base;
  dma->cpu_address = address;
  dma->address = bus;
  dma->read = read;
  dma->done = false;
  dma->access = false;
  dma->reset_used = false;
  dma->info1 = 0;
  dma->info2 = 0;
#ifdef CONFIG_ARCH_DCACHE
  dma->cache_active = cached;
  dma->cache_length = cached ? length : 0;
  if (cached)
    {
      /* Caller relinquishes the whole range through terminal completion.
       * RX clean preserves dirty contents before invalidation; complete
       * lines prevent maintenance from discarding adjacent caller data.
       * Staged transfers maintain only the private staging pool.
       */

      up_clean_dcache(address, address + length);
      if (read)
        {
          up_invalidate_dcache(address, address + length);
        }

      UP_DSB();
    }

#endif
  rzv2h_sdiodma_disable(dma);
  putreg32(0, base + RZV2H_SDHI_DM_CM_INFO1);
  putreg32(0, base + RZV2H_SDHI_DM_CM_INFO2);
  putreg32(SDHI_DM_MODE_64BIT | (read ? SDHI_DM_MODE_RX : 0),
           base + RZV2H_SDHI_DM_CM_DTRAN_MODE);
  putreg32(dma->address, base + RZV2H_SDHI_DM_DTRAN_ADDR);
  putreg16(SDHI_HOST_MODE_64BIT, base + RZV2H_SDHI_HOST_MODE);
  putreg32(~endbit, base + RZV2H_SDHI_DM_CM_INFO1_MASK);
  putreg32(~errbit, base + RZV2H_SDHI_DM_CM_INFO2_MASK);
  putreg16(SDHI_CC_EXT_MODE_RESET | SDHI_CC_EXT_MODE_DMASDRW,
           base + RZV2H_SDHI_CC_EXT_MODE);
  dma->prepared = true;
  return 1;
}

void rzv2h_sdiodma_start(struct rzv2h_sdiodma_s *dma)
{
  if (dma->prepared && !dma->running)
    {
      dma->running = true;
      dma->starts++;
#ifdef CONFIG_RZV2H_SDIO_DMA_BOUNCE
      if (dma->bounced)
        {
          dma->bounce_starts++;
        }

#endif
      UP_DSB();
      putreg32(SDHI_DM_START, dma->base + RZV2H_SDHI_DM_CM_DTRAN_CTRL);
    }
}

int rzv2h_sdiodma_poll(struct rzv2h_sdiodma_s *dma, bool access)
{
  uint32_t endbit = dma->read ? SDHI_DM_END_RX : SDHI_DM_END_TX;

  if (!dma->running)
    {
      return -EINPROGRESS;
    }

  dma->access |= access;
  dma->info1 = getreg32(dma->base + RZV2H_SDHI_DM_CM_INFO1);
  dma->info2 = getreg32(dma->base + RZV2H_SDHI_DM_CM_INFO2);
  if ((dma->info2 & (SDHI_DM_ERROR_RX | SDHI_DM_ERROR_TX)) != 0)
    {
      return -EIO;
    }

  if ((dma->info1 & endbit) != 0)
    {
      /* Preserve other W0C flags, but write reserved bits zero
       * (manual 6.2.2.2.28).
       */

      putreg32(SDHI_DM_CM_INFO1_W0C_MASK & ~endbit,
               dma->base + RZV2H_SDHI_DM_CM_INFO1);
      dma->done = true;
    }

  if (!dma->done || !dma->access)
    {
      return -EINPROGRESS;
    }

  rzv2h_sdiodma_disable(dma);
  UP_DSB();
  dma->running = false;
  dma->prepared = false;
#ifdef CONFIG_ARCH_DCACHE
  rzv2h_sdiodma_cache_complete(dma);
#endif
  dma->completions++;
  return OK;
}

/* Publish staged RX data only after the parent validates the whole transfer,
 * including card/automatic-stop status.  Run before signaling TRANSFERDONE.
 * This may execute in IRQ context; the extra CPU copy is bounded to 4 KiB.
 */

int rzv2h_sdiodma_finish(struct rzv2h_sdiodma_s *dma, int result)
{
#ifdef CONFIG_ARCH_DCACHE
  if (dma->cache_active && (dma->running || dma->prepared))
    {
      /* Parent calls finish before stop on errors. Keep cache ownership
       * attached until stop confirms reset containment and calls us again.
       */

      return result == OK ? -EIO : result;
    }

  rzv2h_sdiodma_cache_complete(dma);
#endif
#ifdef CONFIG_RZV2H_SDIO_DMA_BOUNCE
  if (dma->bounced)
    {
      struct rzv2h_sdiodma_pool_s *pool = rzv2h_sdiodma_pool(dma->base);

      if (dma->running || dma->prepared)
        {
          return result == OK ? -EIO : result;
        }

      if (result == OK)
        {
          if (pool == NULL || pool->owner != dma || dma->running ||
              dma->prepared || !dma->done || !dma->access)
            {
              result = -EIO;
            }
          else
            {
              if (dma->read)
                {
                  UP_DSB();
                  memcpy((void *)dma->bounce_buffer, pool->buffer,
                         dma->bounce_length);
                }

              dma->bounce_completions++;
            }
        }

      if (pool != NULL && pool->owner == dma)
        {
          pool->owner = NULL;
        }

      dma->bounced = false;
      dma->bounce_buffer = NULL;
      dma->bounce_length = 0;
    }
#else
  (void)dma;
#endif

  return result;
}

int rzv2h_sdiodma_stop(struct rzv2h_sdiodma_s *dma)
{
  uint32_t value;

  dma->stop_offset = 0;
  dma->stop_value = 0;
  if (!dma->prepared && !dma->running)
    {
      rzv2h_sdiodma_finish(dma, -ECANCELED);
      return OK;
    }

  if (dma->running)
    {
      /* A failed reset must retain transfer/cache/staging ownership.
       * Return diagnostics to the parent before any terminal cleanup.
       * Successful readback is a software check, not AXI-drain proof.
       */

      /* Stop card communication before resetting DMAC (manual 6.2.2.2.27).
       * The parent latches a fault; do not resume a partly transferred card
       * command or return the caller's buffer to PIO after this abort.
       */

      putreg16(SDHI_SOFT_RST_ASSERT, dma->base + RZV2H_SDHI_SOFT_RST);
      value = getreg16(dma->base + RZV2H_SDHI_SOFT_RST);
      if ((value & SDHI_SOFT_RST_SDRST) != 0)
        {
          dma->stop_offset = RZV2H_SDHI_SOFT_RST;
          dma->stop_value = value;
          return -EIO;
        }

      UP_DSB();
      putreg32(UINT32_MAX & ~SDHI_DM_RST_CHANNELS,
               dma->base + RZV2H_SDHI_DM_CM_RST);
      value = getreg32(dma->base + RZV2H_SDHI_DM_CM_RST);
      if ((value & SDHI_DM_RST_CHANNELS) != 0)
        {
          dma->stop_offset = RZV2H_SDHI_DM_CM_RST;
          dma->stop_value = value;
          return -EIO;
        }

      UP_DSB();
      putreg32(UINT32_MAX, dma->base + RZV2H_SDHI_DM_CM_RST);
      getreg32(dma->base + RZV2H_SDHI_DM_CM_RST);
      dma->reset_used = true;
      dma->errors++;
    }

  rzv2h_sdiodma_disable(dma);
  dma->running = false;
  dma->prepared = false;
  rzv2h_sdiodma_finish(dma, -ECANCELED);
  return OK;
}
