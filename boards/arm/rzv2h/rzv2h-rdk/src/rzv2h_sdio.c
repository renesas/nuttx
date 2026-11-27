/****************************************************************************
 * boards/arm/rzv2h/rzv2h-rdk/src/rzv2h_sdio.c
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

#include <arch/board/board.h>

#include "rzv2h_gpio.h"
#include "rzv2h_sdio.h"
#include "rzv2h-rdk.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if defined(CONFIG_RZV2H_SDIO_CHANNEL1) || \
    defined(CONFIG_RZV2H_SDIO_CHANNEL2)
#  error "RDK socket SD1 is wired only to SDHI0"
#endif

#ifdef CONFIG_RZV2H_SDIO0_MEDIA_EMMC
#  error "RDK SDHI0 socket has no eMMC wiring"
#endif

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

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Configure only the connected SDHI0 signals. This board port does not
 * drive PA0/PA1 as voltage or socket power controls.
 */

static const gpio_pinset_t g_sdhi0_pins[] =
{
  BOARD_SDHI0_CLK,
  BOARD_SDHI0_CMD,
  BOARD_SDHI0_DAT0,
  BOARD_SDHI0_DAT1,
  BOARD_SDHI0_DAT2,
  BOARD_SDHI0_DAT3,
#ifdef CONFIG_RZV2H_SDIO0_WRITEPROTECT_NATIVE
  BOARD_SDHI0_WP,
#endif
#ifdef CONFIG_RZV2H_SDIO0_CARDDECTECT_NATIVE
  BOARD_SDHI0_CD,
#endif
};

#endif /* CONFIG_RZV2H_SDIO_CHANNEL0 */

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
    0},
#else
  {NULL, 0, 0},
#endif
  {NULL, 0, 0}
};

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

/* Initialize each channel once; retry only unfinished registrations. */

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
/* CMD11 and the clock/DAT0 handshake belong to the host. This callback
 * accepts the 1.8 V request without reading or changing the supply and
 * rejects 3.3 V requests. Success does not verify the physical voltage.
 */

int rzv2h_sdio_setvoltage(int channel, bool voltage18)
{
  if (channel != 0 || !voltage18)
    {
      return -ENOTSUP;
    }

  if (up_interrupt_context())
    {
      return -EPERM;
    }

  return OK;
}
#endif

#ifdef CONFIG_RZV2H_SDIO_UHS
/****************************************************************************
 * Name: rzv2h_sdio_prepare_reinsert
 *
 * Description:
 *   Prepare identification after full card removal without supply writes.
 *   The host owns quiescence and native absence checks. This board profile
 *   does not implement voltage or socket power control.
 *   Complete physical removal, held until the host ready notice, is an
 *   operator precondition; native CD alone cannot prove card power loss.
 *   Repeated card/electrical compatibility still requires target acceptance.
 *
 ****************************************************************************/

int rzv2h_sdio_prepare_reinsert(int channel)
{
  const struct rzv2h_sdio_config_s *config;
  int ret = rzv2h_sdio_getconfig(channel, &config);

  if (ret < 0)
    {
      return ret;
    }

  if (up_interrupt_context())
    {
      return -EPERM;
    }

  if (channel != 0 || config->cd_source != RZV2H_CD_NATIVE)
    {
      return -ENOTSUP;
    }

  return OK;
}
#endif
