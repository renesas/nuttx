/****************************************************************************
 * boards/arm/rzv2h/rzv2h-evk/src/rzv2h_sdio.c
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
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/mmcsd.h>
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>
#include <nuttx/sched.h>
#include <nuttx/clock.h>

#include <arch/board/board.h>

#include "rzv2h_gpio.h"
#include "rzv2h_sdio.h"
#include "rzv2h-evk.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_RZV2H_SDIO_CHANNEL2
#  error "EVK has no SDHI2 board resource setup yet"
#endif

#if defined(CONFIG_RZV2H_SDIO_CHANNEL0) && \
    defined(CONFIG_RZV2H_SDIO_CHANNEL1)
#  if CONFIG_RZV2H_SDIO0_INTSEL == CONFIG_RZV2H_SDIO1_INTSEL
#    error "SDHI0 and SDHI1 require distinct INTSEL slots"
#  endif
#  if defined(CONFIG_MMCSD_SDIO) && \
      CONFIG_RZV2H_SDIO0_MINOR == CONFIG_RZV2H_SDIO1_MINOR
#    error "SDHI0 and SDHI1 require distinct block-device minors"
#  endif
#endif

/* Existing pin/power settling policy, in microseconds. */

#define BOARD_SDHI_PIN_SETTLE_US   1000

/* Controlled empty-slot preparation; electrical timing remains
 * unqualified.
 */

#define BOARD_SDHI_REINSERT_OFF_US 100000
#define BOARD_SDHI_REINSERT_ON_US  10000

#ifdef CONFIG_RZV2H_SDIO_CHANNEL0

static const struct rzv2h_sdio_config_s g_sdhi0_config =
{
  .input_clock    = BOARD_SDHI_INPUT_CLOCK,
  .max_frequency  = BOARD_SDHI_MAX_FREQUENCY,
  .max_current_ma = BOARD_SDHI0_MAX_CURRENT_MA,
  .embedded_card  = BOARD_SDHI0_EMBEDDED_CARD,
  .cd_debounce_ms = 50,
  .channel        = 0,
  .intsel         = CONFIG_RZV2H_SDIO0_INTSEL,
  .priority       = CONFIG_RZV2H_SDIO0_IRQ_PRIORITY,
#ifdef CONFIG_RZV2H_SDIO_WIDTH4
  .bus_width      = 4,
#else
  .bus_width      = 1,
#endif
#ifdef CONFIG_RZV2H_SDIO_DMA
  .allow_dma      = true,
#endif
#ifdef CONFIG_RZV2H_SDIO_UHS
  .allow_uhs      = true,
#endif
#if defined(CONFIG_RZV2H_SDIO0_CARDDECTECT_NATIVE)
  .cd_source      = RZV2H_CD_NATIVE,
#else
  .cd_source      = RZV2H_CD_FIXED,
#endif
#ifdef CONFIG_RZV2H_SDIO0_WRITEPROTECT_NATIVE
#  ifndef BOARD_SDHI0_WP
#    error "SDHI0 WP requires a board pin mapping"
#  endif
  .wp_source      = RZV2H_WP_NATIVE,
#  ifdef BOARD_SDHI0_WP_ACTIVE_LOW
  .wp_active_low  = BOARD_SDHI0_WP_ACTIVE_LOW,
#  endif
#endif
};
#endif
#ifdef CONFIG_RZV2H_SDIO_CHANNEL1
static const struct rzv2h_sdio_config_s g_sdhi1_config =
{
  .input_clock    = BOARD_SDHI_INPUT_CLOCK,
  .max_frequency  = BOARD_SDHI_MAX_FREQUENCY,
  .max_current_ma = BOARD_SDHI1_MAX_CURRENT_MA,
  .embedded_card  = BOARD_SDHI1_EMBEDDED_CARD,
  .cd_debounce_ms = 50,
  .channel        = 1,
  .intsel         = CONFIG_RZV2H_SDIO1_INTSEL,
  .priority       = CONFIG_RZV2H_SDIO1_IRQ_PRIORITY,
#ifdef CONFIG_RZV2H_SDIO_WIDTH4
  .bus_width      = 4,
#else
  .bus_width      = 1,
#endif
#ifdef CONFIG_RZV2H_SDIO_DMA
  .allow_dma      = true,
#endif
#ifdef CONFIG_RZV2H_SDIO_UHS
  .allow_uhs      = true,
#endif
#if defined(CONFIG_RZV2H_SDIO1_CARDDETECT_NATIVE)
  .cd_source      = RZV2H_CD_NATIVE,
#else
  .cd_source      = RZV2H_CD_FIXED,
#endif
#ifdef CONFIG_RZV2H_SDIO1_WRITEPROTECT_NATIVE
#  ifndef BOARD_SDHI1_WP
#    error "SDHI1 WP requires a board pin mapping"
#  endif
  .wp_source      = RZV2H_WP_NATIVE,
#  ifdef BOARD_SDHI1_WP_ACTIVE_LOW
  .wp_active_low  = BOARD_SDHI1_WP_ACTIVE_LOW,
#  endif
#endif
};
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Preserve voltage select, signals/card detect, power, then optional WP
 * order. Optional pads remain available to other peripherals when disabled.
 */

#ifdef CONFIG_RZV2H_SDIO_CHANNEL0
static const gpio_pinset_t g_sdhi0_pins[] =
{
  BOARD_SDHI0_VSEL,
  BOARD_SDHI0_CLK,
  BOARD_SDHI0_CMD,
  BOARD_SDHI0_DAT0,
  BOARD_SDHI0_DAT1,
  BOARD_SDHI0_DAT2,
  BOARD_SDHI0_DAT3,
#ifdef CONFIG_RZV2H_SDIO0_CARDDECTECT_NATIVE
  BOARD_SDHI0_CD,
#endif
  BOARD_SDHI0_POWER,
#ifdef CONFIG_RZV2H_SDIO0_WRITEPROTECT_NATIVE
  BOARD_SDHI0_WP,
#endif
};
#endif

#ifdef CONFIG_RZV2H_SDIO_CHANNEL1
static const gpio_pinset_t g_sdhi1_pins[] =
{
  BOARD_SDHI1_VSEL,
  BOARD_SDHI1_CLK,
  BOARD_SDHI1_CMD,
  BOARD_SDHI1_DAT0,
  BOARD_SDHI1_DAT1,
  BOARD_SDHI1_DAT2,
  BOARD_SDHI1_DAT3,
#ifdef CONFIG_RZV2H_SDIO1_CARDDETECT_NATIVE
  BOARD_SDHI1_CD,
#endif
  BOARD_SDHI1_POWER,
#ifdef CONFIG_RZV2H_SDIO1_WRITEPROTECT_NATIVE
  BOARD_SDHI1_WP,
#endif
};
#endif

struct rzv2h_sdio_pinset_s
{
  const gpio_pinset_t *pins;
  unsigned int count;
  unsigned int settle_us;
};

/* Indexed by hardware channel. Empty entries have no board resources. */

static const struct rzv2h_sdio_pinset_s g_sdhi_pinsets[] =
{
#ifdef CONFIG_RZV2H_SDIO_CHANNEL0
  {g_sdhi0_pins, sizeof(g_sdhi0_pins) / sizeof(g_sdhi0_pins[0]),
    BOARD_SDHI_PIN_SETTLE_US},
#else
  {NULL, 0, 0},
#endif
#ifdef CONFIG_RZV2H_SDIO_CHANNEL1
  {g_sdhi1_pins, sizeof(g_sdhi1_pins) / sizeof(g_sdhi1_pins[0]),
    BOARD_SDHI_PIN_SETTLE_US},
#else
  {NULL, 0, 0},
#endif
};

#ifdef CONFIG_RZV2H_SDIO_UHS
/* Reinsertion power controls are independent of optional pin-array
 * entries.
 */

static const gpio_pinset_t g_sdhi_power_pins[] =
{
  BOARD_SDHI0_POWER,
  BOARD_SDHI1_POWER
};
#endif

static bool g_initialized[2];
static bool g_resources_initialized[2];
static mutex_t g_setup_lock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int rzv2h_sdio_configure_pins(unsigned int channel)
{
  const struct rzv2h_sdio_pinset_s *pinset;
  unsigned int i;
  int ret;

  if (channel >= sizeof(g_sdhi_pinsets) / sizeof(g_sdhi_pinsets[0]) ||
      g_sdhi_pinsets[channel].pins == NULL)
    {
      return -ENODEV;
    }

  pinset = &g_sdhi_pinsets[channel];
  for (i = 0; i < pinset->count; i++)
    {
      ret = rzv2h_configgpio(pinset->pins[i]);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (pinset->settle_us > 0)
    {
      up_udelay(pinset->settle_us);
    }

  return OK;
}

static int rzv2h_sdio_resources_initialize(unsigned int channel)
{
  const struct rzv2h_sdio_config_s *config;
  int ret;

  ret = rzv2h_sdio_getconfig(channel, &config);
  if (ret < 0)
    {
      return ret;
    }

  ret = rzv2h_sdio_configure_pins(channel);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: SDHI%u pin/power setup failed: %d\n",
             channel, ret);
      return ret;
    }

  return rzv2h_sdio_enable_cpg(channel);
}

/* Initialize each channel once.  Keep successful registrations alive if
 * another channel fails; a later call retries only unfinished channels.
 */

static int rzv2h_sdio_setup_channel(unsigned int channel, int minor)
{
  const struct rzv2h_sdio_config_s *config;
  struct sdio_dev_s *dev;
  int ret;

  ret = rzv2h_sdio_getconfig(channel, &config);
  if (ret < 0)
    {
      return ret;
    }

  if (g_initialized[channel])
    {
      return OK;
    }

  if (!g_resources_initialized[channel])
    {
      ret = rzv2h_sdio_resources_initialize(channel);
      if (ret < 0)
        {
          return ret;
        }

      g_resources_initialized[channel] = true;
    }

  ret = rzv2h_sdio_initialize(channel, &dev);
  if (ret < 0)
    {
      return ret;
    }

  ret = SDIO_ATTACH(dev);
  if (ret < 0)
    {
      int cleanup = rzv2h_sdio_uninitialize(dev);

      if (cleanup < 0)
        {
          syslog(LOG_ERR, "ERROR: SDHI%u cleanup failed: %d (setup: %d)\n",
                 channel, cleanup, ret);
        }

      return ret;
    }

  /* Native presence filtering may still be settling. Report host readiness
   * here; the deferred media worker reports delivered insertion/removal.
   */

  syslog(LOG_NOTICE, "SDHI%u: host ready\n", channel);

#ifdef CONFIG_MMCSD_SDIO
  ret = mmcsd_slotinitialize(minor, dev);
  if (ret < 0)
    {
      int cleanup = rzv2h_sdio_uninitialize(dev);

      if (cleanup < 0)
        {
          syslog(LOG_ERR, "ERROR: SDHI%u cleanup failed: %d (setup: %d)\n",
                 channel, cleanup, ret);
        }

      return ret;
    }
#endif

  g_initialized[channel] = true;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Board policy lookup; no GPIO, clock, power or controller access. */

int rzv2h_sdio_getconfig(int channel,
                       const struct rzv2h_sdio_config_s **config)
{
  if (config == NULL)
    {
      return -EINVAL;
    }

  *config = NULL;
  if (channel < 0 || channel >= 3)
    {
      return -EINVAL;
    }

  switch (channel)
    {
#ifdef CONFIG_RZV2H_SDIO_CHANNEL0
      case 0:
        *config = &g_sdhi0_config;
        return OK;
#endif
#ifdef CONFIG_RZV2H_SDIO_CHANNEL1
      case 1:
        *config = &g_sdhi1_config;
        return OK;
#endif
      default:
        return -ENODEV;
    }
}

int rzv2h_sdio_setup(void)
{
  static const bool enabled[2] =
  {
#ifdef CONFIG_RZV2H_SDIO_CHANNEL0
    true,
#else
    false,
#endif
#ifdef CONFIG_RZV2H_SDIO_CHANNEL1
    true
#else
    false
#endif
  };

  static const int minors[2] =
  {
#if defined(CONFIG_MMCSD_SDIO) && defined(CONFIG_RZV2H_SDIO_CHANNEL0)
    CONFIG_RZV2H_SDIO0_MINOR,
#else
    0,
#endif
#if defined(CONFIG_MMCSD_SDIO) && defined(CONFIG_RZV2H_SDIO_CHANNEL1)
    CONFIG_RZV2H_SDIO1_MINOR
#else
    0
#endif
  };

  unsigned int channel;
  int result = OK;
  int ret;

  if (up_interrupt_context())
    {
      return -EPERM;
    }

  ret = nxmutex_lock(&g_setup_lock);
  if (ret < 0)
    {
      return ret;
    }

  for (channel = 0; channel < sizeof(enabled) / sizeof(enabled[0]);
       channel++)
    {
      if (enabled[channel])
        {
          ret = rzv2h_sdio_setup_channel(channel, minors[channel]);
          if (ret < 0)
            {
              syslog(LOG_ERR, "ERROR: SDHI%u setup failed: %d\n",
                     channel, ret);
              if (result == OK)
                {
                  result = ret;
                }
            }
        }
    }

  nxmutex_unlock(&g_setup_lock);
  return result;
}

#ifdef CONFIG_RZV2H_SDIO_UHS
/* Renesas EVK pinctrl uses the same pads for default and UHS. PA0/PA2
 * select SDHI0/SDHI1 VQMMC: low=3.3 V, high=1.8 V. Card VMMC stays
 * at 3.3 V (PA1/PA3 power enables). The caller gates that host's SDCLK.
 */

int rzv2h_sdio_setvoltage(int channel, bool voltage18)
{
  switch (channel)
    {
#ifdef CONFIG_RZV2H_SDIO_CHANNEL0
      case 0:
        return rzv2h_gpiowrite(BOARD_SDHI0_VSEL, voltage18);
#endif
#ifdef CONFIG_RZV2H_SDIO_CHANNEL1
      case 1:
        return rzv2h_gpiowrite(BOARD_SDHI1_VSEL, voltage18);
#endif
      default:
        return -ENOTSUP;
    }
}
#endif

#ifdef CONFIG_RZV2H_SDIO_UHS
/* Board preparation: caller verified absence and stopped SDCLK/IRQs.
 * Keep the card physically out until the completion notice. These SD pads
 * are dedicated pins, not GPIO Hi-Z pins. PA power/voltage controls are
 * the retained EVK regulator mapping, not an RDK-compatible service.
 */

int rzv2h_sdio_prepare_reinsert(int channel)
{
  const struct rzv2h_sdio_config_s *config;
  int ret = rzv2h_sdio_getconfig(channel, &config);

  if (ret < 0 || channel > 1)
    {
      return ret < 0 ? ret : -ENOTSUP;
    }

  if (up_interrupt_context())
    {
      return -EPERM;
    }

  if (config->cd_source != RZV2H_CD_NATIVE)
    {
      return -ENOTSUP;
    }

  ret = rzv2h_gpiowrite(g_sdhi_power_pins[channel], false);
  if (ret < 0)
    {
      return ret;
    }

  /* Conservative experimental settling policy, not measured rail proof. */

  nxsched_usleep(BOARD_SDHI_REINSERT_OFF_US + USEC_PER_TICK);
  ret = rzv2h_sdio_setvoltage(channel, false);
  if (ret == OK)
    {
      ret = rzv2h_gpiowrite(g_sdhi_power_pins[channel], true);
    }

  if (ret < 0)
    {
      (void)rzv2h_gpiowrite(g_sdhi_power_pins[channel], false);
      return ret;
    }

  nxsched_usleep(BOARD_SDHI_REINSERT_ON_US + USEC_PER_TICK);
  return OK;
}
#endif
