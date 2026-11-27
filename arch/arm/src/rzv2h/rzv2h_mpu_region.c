/****************************************************************************
 * arch/arm/src/rzv2h/rzv2h_mpu_region.c
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
#include <assert.h>
#include <sys/param.h>
#include <nuttx/cache.h>
#include <arch/board/board.h>
#include "rzv2h_mpu_region.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Index of the executable window inside g_mpu_region_table[].  It must be
 * the LAST entry: in PMSAv7 the highest-numbered matching region wins, and
 * mpu_initialize() programs region N from table entry N, so this entry is
 * what makes the code range executable again on top of the XN default set by
 * MPU_REGION_SRAM.  Reorder the table and the first instruction fetched
 * after mpu_control(true) aborts.
 */

#ifdef CONFIG_RZV2H_DCACHE
#  define MPU_REGION_SRAM 3 /* After the Strongly-ordered default region */
#else
#  define MPU_REGION_SRAM 2
#endif
#define MPU_REGION_CODE (nitems(g_mpu_region_table) - 1)

/* Index of the non-cacheable GBETH DMA window: just below the code window,
 * above MPU_REGION_SRAM so that it overrides the cacheable SRAM default.
 */

#ifdef CONFIG_RZV2H_GBETH
#  define MPU_REGION_DMA  (nitems(g_mpu_region_table) - 2)
#endif

/****************************************************************************
 * External Symbols
 ****************************************************************************/

/* Bounds of the read-only part of the image, from the linker script.  The
 * script pads __mpu_code_end up to a power of two so that the region below
 * is an exact fit with no sub-region mask; see the ALIGN(1 << LOG2CEIL(...))
 * in boards/arm/rzv2h/rzv2h-evk/scripts/rzv2h-evk_cr8_0.ld.
 */

extern uint8_t __mpu_code_start[];
extern uint8_t __mpu_code_end[];

/* Bounds of the GBETH DMA memory (descriptors and packet buffers), padded
 * by the linker script to a power of two and aligned to its size.
 */

#ifdef CONFIG_RZV2H_GBETH
extern uint8_t __dma_start[];
extern uint8_t __dma_end[];
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* MPU definition table for CR8 core0.
 *
 * Not const: the size of the last entry is only known from the link map and
 * is filled in by rzv2h_mpuinit() before the table is programmed.
 */

struct mpu_region_s g_mpu_region_table[] =
{
#ifdef CONFIG_RZV2H_DCACHE
  /* Default for 0x00000000..0x7FFFFFFF (lowest priority)
   *
   * mpu_control() enables the background region (SCTLR.BR), and the
   * PMSAv7 default map makes every unmapped address below 0x80000000
   * Normal memory, i.e. cacheable once the D-cache is on.  The CR8 may then
   * fill cache lines speculatively from DDR (0x40000000, not initialized
   * in a debugger-only boot) or other unbacked space, and a fill that never
   * completes locks the bus.  Strongly-ordered (TEX=000, C=0, B=0) keeps
   * unmapped addresses accessible as before but never cached or read
   * speculatively.  Every region below has a higher number and overrides
   * it; with this entry their numbers in the comments shift up by one.
   */

  {
    0x00000000,                                        /* base address */
    MPU_SIZE_MB(2048),                                 /* size */
    MPU_RACR_AP_RWRW | MPU_RACR_XN                     /* attribute */
  },
#endif

  /* region 0 (ITCM)
   *
   * Read-only, and deliberately NOT XN: this is where the exception table
   * lives.  rzv2h_vectorsetup() copies the table here from arm_boot(), which
   * runs before rzv2h_mpuinit(), so write access is no longer needed by the
   * time this region takes effect and a stray write can no longer corrupt
   * the vectors.
   */

  {
    0x00000000,                                        /* base address */
    MPU_SIZE_KB(128),                                  /* size */
    MPU_RACR_AP_RORO | MPU_RACR_TEX(1)                 /* attribute */
  },

  /* region 1 (DTCM) */

  {
    0x00020000,                                        /* base address */
    MPU_SIZE_KB(128),                                  /* size */
    MPU_RACR_AP_RWRW | MPU_RACR_TEX(1) | MPU_RACR_XN   /* attribute */
  },

  /* region 2 for core0 (RCPU-SRAM Cacheable)
   *
   * The SRAM image lives here - .text, .data, .bss, the IDLE stack and the
   * heap are all inside 0x08180000..0x081FFFFF.  Both build types also keep
   * the load copy of the vector table here; the table itself runs in ITCM.
   * This entry is the data default: read/write and XN, so nothing in the
   * heap, in a stack or in .data can ever be executed.
   *
   * XN is only safe because MPU_REGION_CODE below re-maps the code range as
   * executable, and being a higher-numbered region it wins where the two
   * overlap.  Without that overlay, the first instruction fetch after
   * mpu_control(true) writes SCTLR_M faults, which looks like
   * "cp15_wrsctlr() crashed" but is really a prefetch abort on the NOPs
   * immediately following the mcr [armv7-r/sctlr.h:523].
   *
   * Cacheable as in the Renesas CR8 table: inner write-back, outer
   * non-cacheable (RZV2H_MPU_NORMAL_CACHED).  Outer write-back attributes
   * are never used by the vendor on this bus.
   */

  {
    0x08180000,                                        /* base address */
    MPU_SIZE_KB(512),                                  /* size */
    MPU_RACR_AP_RWRW | RZV2H_MPU_NORMAL_CACHED |
    MPU_RACR_XN                                        /* attribute */
  },

  /* region 3 (Peripheral)
   *
   * Device memory (S | B, no TEX/C), so MMIO is neither cached nor
   * reordered into the caches once they are enabled at the end of
   * rzv2h_mpuinit(), and XN because nothing is ever fetched from here.
   */

  {
    0x10000000,                                        /* base address */
    MPU_SIZE_MB(256),                                  /* size */
    MPU_RACR_AP_RWRW |
    MPU_RACR_S | MPU_RACR_B | MPU_RACR_XN              /* attribute */
  },

  /* region 4 (xSPI)
   *
   * The flash window is read-only, execute-never, and Normal
   * non-cacheable (TEX=001, C=0, B=0). This prevents D-cache line fills
   * from the window, including before the xSPI controller is initialized.
   * Flash programming uses DirectTransfer commands.
   *
   * This entry is always present and takes priority over the
   * Strongly-ordered default region when D-cache is enabled.
   */

  {
    0x20000000,                                        /* base address */
    MPU_SIZE_MB(256),                                  /* size */
    MPU_RACR_AP_RORO | MPU_RACR_TEX(1) | MPU_RACR_XN   /* attribute */
  },

#ifdef CONFIG_RZV2H_RPTUN
  /* region 5 (OpenAMP resource table and MHU shared memory)
   *
   * Normal, non-cacheable, shareable memory, matching regions 8 and 9 in
   * the Renesas CR8_0 reference project.
   */

  {
    BOARD_RPTUN_RESOURCE_ADDR,                         /* base address */
    MPU_SIZE_KB(8),                                    /* size */
    MPU_RACR_AP_RWRW | MPU_RACR_TEX(1) | MPU_RACR_S |
    MPU_RACR_XN                                        /* attribute */
  },

  /* region 6 (OpenAMP vrings and RPMsg buffers) */

  {
    BOARD_RPTUN_RPMSG_ADDR,                            /* base address */
    BOARD_RPTUN_RPMSG_MPU_SIZE,                        /* size */
    MPU_RACR_AP_RWRW | MPU_RACR_TEX(1) | MPU_RACR_S |
    MPU_RACR_XN                                        /* attribute */
  },
#endif

#ifdef CONFIG_RZV2H_GBETH
  /* GBETH DMA window (the descriptor rings, the .dma section)
   *
   * Normal, non-cacheable (TEX=001, C=0, B=0) and XN, so the CPU and the
   * GBETH DMA always see the same descriptors with the D-cache on.  The
   * packet buffers are cacheable and maintained by the driver.  It lies
   * inside the SRAM region and after the code window, never overlapping
   * it.  Base and size are filled in from the link map by rzv2h_mpuinit().
   */

  {
    0,                                                 /* base: link map */
    0,                                                 /* size: link map */
    MPU_RACR_AP_RWRW | MPU_RACR_TEX(1) | MPU_RACR_XN   /* attribute */
  },
#endif

  /* Last region (SRAM code window: .text, .rodata, .init_section,
   * .ARM.exidx, and the .vectors load image)
   *
   * Read-only and executable, cacheable like region 2.  Base and size are
   * filled in from the link map by rzv2h_mpuinit(); see the note on
   * MPU_REGION_CODE above for why this has to stay last.
   */

  {
    0,                                                 /* base: link map */
    0,                                                 /* size: link map */
    MPU_RACR_AP_RORO | RZV2H_MPU_NORMAL_CACHED         /* attribute */
  }
};
 #ifdef CONFIG_ARM_MPU
static_assert(nitems(g_mpu_region_table) <= CONFIG_ARM_MPU_NREGIONS,
              "g_mpu_region_table has more entries than "
              "CONFIG_ARM_MPU_NREGIONS");
#endif
#ifdef CONFIG_ARM_MPU
/* MPUIR as read at boot.  Kept around so the region count and the
 * MPUIR_SEPARATE bit can be inspected from the debugger without CP15 access:
 * a separate instruction/data map would mean mpu_modify_region() only
 * programs the data side and instruction fetches are left to the background
 * region.
 */

unsigned int g_rzv2h_mpuir;
#endif
/****************************************************************************
 * Public Functions
 ****************************************************************************/

void rzv2h_mpuinit(void)
{
#ifdef CONFIG_ARM_MPU
  unsigned int nregions;
  unsigned int region;

  g_rzv2h_mpuir = mpu_get_mpuir();
  nregions = (g_rzv2h_mpuir & MPUIR_DREGION_MASK) >> MPUIR_DREGION_SHIFT;

  /* Disable every region before programming our own.
   *
   * NuttX only configures CONFIG_ARM_MPU_NREGIONS entries, so anything the
   * bootloader left in the higher-numbered regions survives.  In PMSAv7 the
   * highest-numbered matching region wins, so a leftover region covering
   * 0x00000000 with XN (or no access) silently overrides region 0 (ITCM) and
   * makes the exception table unfetchable - every fault then turns into a
   * prefetch abort loop on the abort vector itself.
   */

  mpu_control(false);

  for (region = 0; region < nregions; region++)
    {
      mpu_set_rgnr(region);
      mpu_set_drsr(0);
    }

  /* A separate instruction/data map would mean mpu_modify_region() only
   * programs the data side (DRBAR/DRSR/DRACR), leaving instruction fetches
   * to the background region - the XN on the SRAM region and the executable
   * window below would both have no effect.
   */

  DEBUGASSERT((g_rzv2h_mpuir & MPUIR_SEPARATE) == 0);
  DEBUGASSERT(nregions >= nitems(g_mpu_region_table));

  /* Take the bounds of the executable window from the link map so that the
   * region can never drift away from what was actually linked.  The linker
   * script has already rounded the size up to a power of two, so
   * mpu_log2regionceil() is exact and mpu_subregion() returns an empty mask.
   */

  g_mpu_region_table[MPU_REGION_CODE].base = (uintptr_t)__mpu_code_start;
  g_mpu_region_table[MPU_REGION_CODE].size = __mpu_code_end -
                                             __mpu_code_start;

  DEBUGASSERT(g_mpu_region_table[MPU_REGION_CODE].base ==
              g_mpu_region_table[MPU_REGION_SRAM].base);
  DEBUGASSERT(g_mpu_region_table[MPU_REGION_CODE].size <=
              g_mpu_region_table[MPU_REGION_SRAM].size);

#ifdef CONFIG_RZV2H_GBETH
  /* The linker script pads .dma to a power of two and asserts that its base
   * is a multiple of that size, so this is an exact PMSAv7 region.
   */

  g_mpu_region_table[MPU_REGION_DMA].base = (uintptr_t)__dma_start;
  g_mpu_region_table[MPU_REGION_DMA].size = __dma_end - __dma_start;

  DEBUGASSERT((g_mpu_region_table[MPU_REGION_DMA].size &
               (g_mpu_region_table[MPU_REGION_DMA].size - 1)) == 0);
  DEBUGASSERT((g_mpu_region_table[MPU_REGION_DMA].base &
               (g_mpu_region_table[MPU_REGION_DMA].size - 1)) == 0);
#endif

  mpu_initialize(&g_mpu_region_table[0], nitems(g_mpu_region_table));

  /* Enable caches after MPU configuration. MMIO regions must be
   * non-cacheable before D-cache is turned on, and DMA memory must be
   * non-cacheable (GBETH: the .dma region above) because no driver does
   * cache maintenance.  up_enable_dcache() invalidates the cache first.
   */

  up_invalidate_icache_all();
  up_enable_icache();

#ifdef CONFIG_RZV2H_DCACHE
  up_enable_dcache();
#endif

#endif
}
