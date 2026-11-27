/****************************************************************************
 * arch/arm/src/rzv2h/rzv2h_sdio.h
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

#ifndef __ARCH_ARM_SRC_RZV2H_RZV2H_SDIO_H
#define __ARCH_ARM_SRC_RZV2H_RZV2H_SDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/* The current host implements SD-memory transactions on an exclusively
 * owned controller. Reject unsupported services even in stale or manually
 * supplied configurations; the MMC/SD caller ignores a failed bus lock.
 */

#if defined(CONFIG_RZV2H_SDIO) && defined(CONFIG_SDIO_MUXBUS)
#  error "RZ/V2H SDHI does not support CONFIG_SDIO_MUXBUS"
#endif

#if defined(CONFIG_RZV2H_SDIO) && defined(CONFIG_RZV2H_SDIO_CARD)
#  error "RZ/V2H SDHI does not implement SDIO peripheral function support"
#endif

#include <nuttx/sdio.h>
#include <stdint.h>
#include <stdbool.h>

/* Architecture-private diagnostic snapshots. These values describe tracked
 * negotiation state, not a measured clock. Keep them out of the public chip
 * include tree; common MMC/SD does not depend on these queries.
 */

enum rzv2h_sdio_mode_e
{
  RZV2H_SDIO_MODE_UNKNOWN = 0,
  RZV2H_SDIO_MODE_DEFAULT,
  RZV2H_SDIO_MODE_HIGH_SPEED,
  RZV2H_SDIO_MODE_UHS_SDR12,
  RZV2H_SDIO_MODE_UHS_SDR50,
  RZV2H_SDIO_MODE_UHS_SDR104
};

/* Task-context, read-only snapshots; no MMIO, commands or mode changes.
 * Return -EINVAL for invalid arguments, -ENODEV before initialization,
 * -EPERM in interrupt context, or the retained host fault. Outputs remain
 * unchanged on error. getcurrent() returns -ENODATA until UHS preparation
 * completes or after removal; it reports allowance, not measured current.
 */

int rzv2h_sdio_getmode(int channel, enum rzv2h_sdio_mode_e *mode);
int rzv2h_sdio_getcurrent(int channel, unsigned int *milliamps);

/* Immutable board slot policy. Board source implements getconfig(), as
 * RZ/V2H SPI board source implements its architecture-declared callbacks.
 * Rates are Hz; current is the supply budget, not measured consumption.
 * Feature permissions describe this build's board profile. The controller
 * validates them before MMIO and retains a private copy for its lifetime.
 */

enum rzv2h_cd_source_e
{
  RZV2H_CD_NATIVE = 0,
  RZV2H_CD_FIXED
};

enum rzv2h_wp_source_e
{
  RZV2H_WP_NONE = 0,
  RZV2H_WP_NATIVE
};

struct rzv2h_sdio_config_s
{
  uint32_t input_clock;
  uint32_t max_frequency;
  unsigned int max_current_ma;
  uint16_t cd_debounce_ms;
  uint8_t bus_width;
  bool allow_dma;
  bool allow_uhs;
  bool embedded_card;
  uint16_t intsel;
  uint8_t channel;
  uint8_t priority;
  enum rzv2h_cd_source_e cd_source;
  enum rzv2h_wp_source_e wp_source;
  bool wp_active_low;
};

/* Task context. Return a static const enabled-slot record, or a negative
 * errno with *config == NULL. This lookup must not change hardware/state.
 */

int rzv2h_sdio_getconfig(int channel,
                       const struct rzv2h_sdio_config_s **config);

/* Task context with exclusive channel ownership. Enable SDHI0/1 CPG module
 * clocks/bus and release module reset; return zero or a negative errno.
 * Disabled/invalid channels return -ENODEV before MMIO. No pin/card-power,
 * SDHI soft
 * reset, card-session or MMCSD registration operations are performed.
 */

int rzv2h_sdio_enable_cpg(unsigned int channel);

/* Before initialize(), the board must exclusively own the channel, enable
 * its clocks, release reset, configure pads/power and permit CR8 MMIO
 * access.
 * EVK discovery starts at 3.3 V. The experimental RDK manual profile has
 * fixed 1.8 V I/O; cold-start/card compatibility is not qualified by this
 * software integration. Board-owned slot records supply IMCLK, clock limits,
 * wiring and integration policy. Disabled channels return -ENODEV.
 * CONFIG_RZV2H_SDIOx_WRITEPROTECT enables native write protection:
 * WRITEPROTECT_NATIVE uses the board-defined SDWP input, configured before
 * initialization. BOARD_SDHIx_WP_ACTIVE_LOW=1 selects LOW=protected; the
 * default is HIGH=protected. Select native only with a wired WP source.
 * CONFIG_RZV2H_SDIOx_CARDDECTECT enables native card detection;
 * disabling it assumes present media and suppresses CD events/pin setup.
 * This does not prepare board resources or register block devices.
 */

/* Serialize calls with dev->mutex. Initialization is task-context only;
 * repeated same-channel initialization returns the existing device without
 * resetting it. After teardown, retained UHS or failed-HS recovery
 * restrictions cause initialization to return -EIO before clearing state
 * or accessing MMIO.
 *
 * SD-memory PIO supports single/multiple blocks in one-bit or negotiated
 * four-bit mode. Default/High Speed use up to 25/50 MHz respectively.
 * Optional socket UHS-I adds SDR50/SDR104 at 1.8 V (EVK SDHI0/1, RDK SDHI0);
 * target acceptance remains pending. Four-bit boards must route DAT0..DAT3.
 * CD notifications use HPWORK when enabled. Optional WP uses the
 * native input with board polarity. Optional socket DMA requires
 * eligible SRAM3 buffers and runtime D-cache disabled.
 */

int rzv2h_sdio_initialize(int channel, struct sdio_dev_s **dev);

#ifdef CONFIG_RZV2H_SDIO_UHS
/* Board voltage service. Called in task context with SDCLK gated.
 * EVK SDHI1 controls VQMMC. RDK SDHI0 has fixed 1.8 V I/O and rejects a
 * 3.3 V request. Does not power-cycle or enumerate a card.
 */

int rzv2h_sdio_setvoltage(int channel, bool voltage18);

/* Prepare the board's identification profile after complete card removal.
 * Caller owns host quiescence and native-CD absence checks; task context,
 * SDCLK/host IRQs stopped, card fully held out until the ready notice.
 * EVK restores slot power and 3.3 V I/O. RDK keeps its fixed 1.8 V I/O
 * and performs no supply write. Success confirms board preparation only;
 * the host must still observe later insertion and start a new session.
 * This is not an inserted-card power-cycle or application restart API.
 */

int rzv2h_sdio_prepare_reinsert(int channel);
#endif

/* Release an instance only after all users have stopped.
 * Does not gate board-owned clocks or remove board-owned pad/power setup.
 * Successful software cleanup does not recover an external card session.
 * UHS/failed-HS restrictions survive teardown and block reinitialization;
 * board/card recovery and a fresh firmware lifetime are still required.
 * If controller stop cannot be verified, retain ownership and return the
 * failure for a safe retry. Interrupt-context calls return -EPERM.
 */

int rzv2h_sdio_uninitialize(struct sdio_dev_s *dev);

#endif /* __ARCH_ARM_SRC_RZV2H_RZV2H_SDIO_H */
